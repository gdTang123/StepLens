#include "../core/TempDir.h"
#include "core/CancelToken.h"
#include "core/LifecycleTrace.h"
#include "core/Log.h"
#include "core/OcctLoadContract.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace {
namespace model = steplens::model;
std::atomic<bool> enqueueGateEntered{false};
std::atomic<bool> releaseEnqueueGate{false};
void blockProducerBeforeNotify() {
    steplens::lifecycleTrace("producer-enqueue-gate");
    enqueueGateEntered.store(true, std::memory_order_release);
    while (!releaseEnqueueGate.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    steplens::lifecycleTrace("producer-enqueue-gate-release");
}
void releaseProducerAfterJoin() {
    releaseEnqueueGate.store(true, std::memory_order_release);
}
std::shared_ptr<const steplens::model::DocumentModel> makeModel() {
    using namespace steplens;
    io::LoadedDocument loaded;
    {
        std::lock_guard lock(xcafDocumentMutex());
        XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", loaded.doc);
        XCAFDoc_DocumentTool::ShapeTool(loaded.doc->Main())->AddShape(BRepPrimAPI_MakeBox(1, 1, 1).Shape());
    }
    loaded.schema = "STEP";
    loaded.lengthUnitToMm = 1;
    loaded.fileBytes = 1;
    steplens::ProgressSink progress;
    steplens::CancelToken cancel;
    auto result = model::DocumentModel::build(std::move(loaded), progress, cancel);
    return result.ok() ? std::move(result.value()) : nullptr;
}

steplens::io::LoadedDocument makeLoadedDocument() {
    using namespace steplens;
    io::LoadedDocument loaded;
    {
        std::lock_guard lock(xcafDocumentMutex());
        XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", loaded.doc);
        XCAFDoc_DocumentTool::ShapeTool(loaded.doc->Main())->AddShape(BRepPrimAPI_MakeBox(1, 1, 1).Shape());
    }
    loaded.schema = "STEP";
    loaded.lengthUnitToMm = 1;
    loaded.fileBytes = 1;
    return loaded;
}

int documentCount() {
    std::lock_guard lock(steplens::xcafDocumentMutex());
    return XCAFApp_Application::GetApplication()->NbDocuments();
}
} // namespace

TEST(DocumentRelease, FirstWaitBeforeDocumentStillDrainsToBaseline) {
    std::atomic<bool> start{false};
    std::atomic<bool> waitResult{false};
    std::atomic<int> observedPending{-1};
    std::thread waiter([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        waitResult.store(model::waitForDocumentRelease(std::chrono::seconds(10)), std::memory_order_release);
    });
    std::thread observer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        observedPending.store(model::documentsPendingRelease(), std::memory_order_release);
    });
    start.store(true, std::memory_order_release);
    waiter.join();
    observer.join();
    ASSERT_TRUE(waitResult.load(std::memory_order_acquire));
    EXPECT_EQ(observedPending.load(std::memory_order_acquire), 0);
    const int baseline = documentCount();
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    docModel.reset();
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(10)));
    EXPECT_EQ(documentCount(), baseline);
}

TEST(DocumentRelease, ControlBlockFailureAfterDetachUsesDeleterOnce) {
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    const int countBefore = documentCount();
    steplens::ProgressSink progress;
    steplens::CancelToken cancel;
    const auto caller = std::this_thread::get_id();
    std::thread::id destroyed;
    int probeCount = 0;
    model::setDocumentReleaseProbe([&](std::thread::id id) {
        destroyed = id;
        ++probeCount;
    });
    model::failNextDocumentControlBlockForTesting(true);
    auto result = model::DocumentModel::build(makeLoadedDocument(), progress, cancel);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    EXPECT_EQ(documentCount(), countBefore);
    EXPECT_EQ(probeCount, 1);
    EXPECT_NE(destroyed, caller);
    EXPECT_TRUE(model::lastDocumentControlBlockAllocatorFailed());
    model::setDocumentReleaseProbe({});
}

TEST(DocumentRelease, CloseFailureIsReportedAndReleaseStillSettles) {
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    steplens::test::TempDir logDir;
    ASSERT_TRUE(steplens::Log::initialize(logDir.path()));
    struct LogGuard {
        ~LogGuard() {
            steplens::Log::shutdown();
        }
    } logGuard;
    const auto failuresBefore = model::documentCloseFailureCount();
    const int documentsBefore = documentCount();
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    const auto caller = std::this_thread::get_id();
    std::thread::id destroyed;
    int probeCount = 0;
    model::setDocumentReleaseProbe([&](std::thread::id id) {
        destroyed = id;
        ++probeCount;
    });
    model::failNextDocumentCloseForTesting(true);
    docModel.reset();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    EXPECT_TRUE(model::lastDocumentCloseFailed());
    EXPECT_EQ(model::documentCloseFailureCount(), failuresBefore + 1);
    EXPECT_EQ(documentCount(), documentsBefore + 1);
    EXPECT_EQ(probeCount, 1);
    EXPECT_NE(destroyed, caller);
    auto nextModel = makeModel();
    ASSERT_TRUE(nextModel);
    nextModel.reset();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_FALSE(model::lastDocumentCloseFailed());
    EXPECT_EQ(model::documentCloseFailureCount(), failuresBefore + 1);
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    EXPECT_EQ(documentCount(), documentsBefore + 1);
    steplens::Log::shutdown();
    std::ifstream logFile(logDir.path() / "steplens.log");
    const std::string logged((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    EXPECT_NE(logged.find("XCAF document Close failed"), std::string::npos) << logged;
    model::setDocumentReleaseProbe({});
}

TEST(DocumentRelease, BusyReleaseQuickExitRunsInChildProcess) {
#if defined(_WIN32)
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = "\"" STEPLENS_RELEASE_QUICK_EXIT_EXE "\"";
    ASSERT_TRUE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process));
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 500);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 9);
    }
    DWORD exitCode = 9;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    EXPECT_EQ(waitResult, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u);
#else
    const pid_t child = fork();
    if (child < 0) {
        FAIL() << "fork failed";
    }
    if (child == 0) {
        execl(STEPLENS_RELEASE_QUICK_EXIT_EXE, STEPLENS_RELEASE_QUICK_EXIT_EXE, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exited) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    EXPECT_TRUE(exited);
    ASSERT_TRUE(exited);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}

TEST(DocumentRelease, HandoffDoesNotTicketAndRetireIsOnce) {
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    model::handoffDocument(docModel);
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    model::retireDocument(docModel);
    model::retireDocument(docModel);
    EXPECT_EQ(model::documentsPendingRelease(), 1);
    EXPECT_FALSE(model::waitForDocumentRelease(std::chrono::milliseconds(10)));
    docModel.reset();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
}
TEST(DocumentRelease, LastUnretiredReferenceIsReleasedOffCallerThread) {
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    const auto caller = std::this_thread::get_id();
    std::thread::id destroyed;
    model::setDocumentReleaseProbe([&](std::thread::id id) { destroyed = id; });
    docModel.reset();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_NE(destroyed, caller);
    model::setDocumentReleaseProbe({});
}

TEST(DocumentRelease, ConcurrentLastReferencesDrainOffCallerThreads) {
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    const int baselineDocuments = documentCount();
    constexpr int threadCount = 8;
    constexpr int modelsPerThread = 4;
    constexpr int totalModels = threadCount * modelsPerThread;
    std::atomic<int> probeCount{0};
    std::atomic<bool> allBackground{true};
    const auto caller = std::this_thread::get_id();
    model::setDocumentReleaseProbe([&](std::thread::id id) {
        if (id == caller) {
            allBackground.store(false, std::memory_order_relaxed);
        }
        probeCount.fetch_add(1, std::memory_order_relaxed);
    });
    std::vector<std::shared_ptr<const steplens::model::DocumentModel>> models;
    models.reserve(totalModels);
    for (int i = 0; i < totalModels; ++i) {
        auto item = makeModel();
        ASSERT_TRUE(item);
        models.push_back(std::move(item));
    }
    std::mutex producerMutex;
    std::vector<std::thread::id> producerIds;
    std::vector<std::thread> workers;
    for (int thread = 0; thread < threadCount; ++thread) {
        workers.emplace_back([&, thread] {
            {
                std::lock_guard lock(producerMutex);
                producerIds.push_back(std::this_thread::get_id());
            }
            for (int index = thread; index < totalModels; index += threadCount) {
                models[index].reset();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(probeCount.load(), totalModels);
    EXPECT_TRUE(allBackground.load());
    const auto workerThread = model::documentReleaseWorkerThread();
    EXPECT_NE(workerThread, caller);
    for (const auto id : producerIds) {
        EXPECT_NE(workerThread, id);
    }
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    EXPECT_EQ(documentCount(), baselineDocuments);
    model::setDocumentReleaseProbe({});
}

TEST(DocumentRelease, NullIsNoopAndUnretiredLastReferenceIsTicketed) {
    model::retireDocument(nullptr);
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    docModel.reset();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
}

TEST(DocumentRelease, RetiredReferenceKeepsTicketUntilItIsReleased) {
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    model::retireDocument(docModel);
    model::retireDocument(docModel);
    EXPECT_EQ(model::documentsPendingRelease(), 1);
    EXPECT_FALSE(model::waitForDocumentRelease(std::chrono::milliseconds(10)));
    docModel.reset();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
}

TEST(DocumentRelease, ConcurrentDuplicateRetireNeverPublishesTwoTickets) {
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    model::retireDocument(docModel);
    constexpr int threadCount = 16;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<int> maximumPending{0};
    std::atomic<int> closeCount{0};
    const auto caller = std::this_thread::get_id();
    std::thread::id destroyed;
    std::mutex producerMutex;
    std::vector<std::thread::id> producerIds;
    model::setDocumentReleaseProbe([&](std::thread::id id) {
        destroyed = id;
        closeCount.fetch_add(1, std::memory_order_relaxed);
    });
    std::thread observer([&] {
        while (!done.load(std::memory_order_acquire)) {
            const int pending = model::documentsPendingRelease();
            int observed = maximumPending.load(std::memory_order_relaxed);
            while (observed < pending &&
                   !maximumPending.compare_exchange_weak(observed, pending, std::memory_order_relaxed)) {
            }
        }
    });
    std::vector<std::thread> producers;
    for (int i = 0; i < threadCount; ++i) {
        producers.emplace_back([copy = docModel, &start, &producerMutex, &producerIds]() mutable {
            {
                std::lock_guard lock(producerMutex);
                producerIds.push_back(std::this_thread::get_id());
            }
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            model::retireDocument(std::move(copy));
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }
    done.store(true, std::memory_order_release);
    observer.join();
    EXPECT_LE(maximumPending.load(), 1);
    EXPECT_EQ(model::documentsPendingRelease(), 1);
    docModel.reset();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    EXPECT_EQ(closeCount.load(), 1);
    const auto workerThread = model::documentReleaseWorkerThread();
    EXPECT_EQ(destroyed, workerThread);
    EXPECT_NE(destroyed, caller);
    for (const auto id : producerIds) {
        EXPECT_NE(destroyed, id);
    }
    model::setDocumentReleaseProbe({});
}

TEST(DocumentRelease, EnqueueNotifyIsCoveredByInFlightAcrossStop) {
    steplens::test::TempDir dir;
    const auto logPath = dir.path() / "lifecycle-r1.log";
    const char* childMark = std::getenv("STEPLENS_R1_ENQUEUE_CHILD");
    if (childMark != nullptr && *childMark != '\0') {
        enqueueGateEntered.store(false, std::memory_order_release);
        releaseEnqueueGate.store(false, std::memory_order_release);
        steplens::initializeLifecycleTrace();
        model::setDocumentReleaseEnqueueGate(blockProducerBeforeNotify);
        model::setDocumentReleaseAfterJoinHookForTesting(releaseProducerAfterJoin);
        if (!model::waitForDocumentRelease(std::chrono::seconds(10))) {
            std::exit(3);
        }
        auto* held = new std::shared_ptr<const model::DocumentModel>(makeModel());
        ASSERT_TRUE(*held);
        std::thread([held] {
            held->reset();
            delete held;
        }).detach();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!enqueueGateEntered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!enqueueGateEntered.load(std::memory_order_acquire)) {
            steplens::lifecycleTrace("r1-child-gate-timeout");
            std::exit(2);
        }
        steplens::lifecycleTrace("r1-child-returning-to-static-dtors");
        return;
    }

#if defined(_WIN32)
    const char* exe = nullptr;
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    exe = module;
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
#endif
#if defined(_WIN32)
    SetEnvironmentVariableA("STEPLENS_R1_ENQUEUE_CHILD", "1");
    SetEnvironmentVariableA("STEPLENS_LIFECYCLE_LOG", logPath.string().c_str());
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module +
                          "\" --gtest_filter=DocumentRelease.EnqueueNotifyIsCoveredByInFlightAcrossStop";
    ASSERT_TRUE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process));
    SetEnvironmentVariableA("STEPLENS_R1_ENQUEUE_CHILD", nullptr);
    SetEnvironmentVariableA("STEPLENS_LIFECYCLE_LOG", nullptr);
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = 9;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 9);
    } else {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    EXPECT_EQ(waitResult, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u);
#else
    setenv("STEPLENS_R1_ENQUEUE_CHILD", "1", 1);
    setenv("STEPLENS_LIFECYCLE_LOG", logPath.string().c_str(), 1);
    const pid_t child = fork();
    if (child < 0) {
        unsetenv("STEPLENS_R1_ENQUEUE_CHILD");
        unsetenv("STEPLENS_LIFECYCLE_LOG");
        FAIL() << "fork failed";
    }
    if (child == 0) {
        setenv("ASAN_OPTIONS", "halt_on_error=1:detect_leaks=0", 1);
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.EnqueueNotifyIsCoveredByInFlightAcrossStop",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    unsetenv("STEPLENS_R1_ENQUEUE_CHILD");
    unsetenv("STEPLENS_LIFECYCLE_LOG");
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exited) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    EXPECT_TRUE(exited);
    ASSERT_TRUE(exited);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
    ASSERT_TRUE(std::filesystem::is_regular_file(logPath)) << logPath;
    std::ifstream in(logPath);
    const std::string logged((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(logged.find("producer-enqueue-gate"), std::string::npos) << logged;
    EXPECT_NE(logged.find("r1-child-returning-to-static-dtors"), std::string::npos) << logged;
    EXPECT_NE(logged.find("retirer-stop"), std::string::npos) << logged;
    const auto notifyAt = logged.find("producer-notify-returned");
    const auto inflightAt = logged.find("producer-inflight-release");
    const auto endAt = logged.find("retirer-end");
    const auto stopAt = logged.find("retirer-stop");
    const auto joinAt = logged.find("retirer-join-end");
    const auto gateAt = logged.find("producer-enqueue-gate");
    const auto gateReleaseAt = logged.find("producer-enqueue-gate-release");
    ASSERT_NE(notifyAt, std::string::npos) << logged;
    ASSERT_NE(inflightAt, std::string::npos) << logged;
    ASSERT_NE(endAt, std::string::npos) << logged;
    ASSERT_NE(stopAt, std::string::npos) << logged;
    ASSERT_NE(joinAt, std::string::npos) << logged;
    ASSERT_NE(gateAt, std::string::npos) << logged;
    ASSERT_NE(gateReleaseAt, std::string::npos) << logged;
    EXPECT_LT(notifyAt, endAt);
    EXPECT_LT(inflightAt, endAt);
    EXPECT_LT(stopAt, gateReleaseAt);
    EXPECT_LT(joinAt, gateReleaseAt);
    EXPECT_LT(gateAt, gateReleaseAt);
    EXPECT_LT(gateReleaseAt, notifyAt);
    // producer-inflight-release is traced just before fetch_sub; retirer-end follows the
    // destructor's actual zero wait, so this marker is not interpreted as the decrement.
    EXPECT_LT(notifyAt, inflightAt);
}

TEST(DocumentRelease, ReleaseDelayKeepsPendingTicketUntilCloseCompletes) {
#if defined(_WIN32)
    SetEnvironmentVariableA("STEPLENS_RELEASE_DELAY_MS", "200");
#else
    setenv("STEPLENS_RELEASE_DELAY_MS", "200", 1);
#endif
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    docModel.reset();
    EXPECT_GT(model::documentsPendingRelease(), 0);
    EXPECT_FALSE(model::waitForDocumentRelease(std::chrono::milliseconds(50)));
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
#if defined(_WIN32)
    SetEnvironmentVariableA("STEPLENS_RELEASE_DELAY_MS", nullptr);
#else
    unsetenv("STEPLENS_RELEASE_DELAY_MS");
#endif
}

TEST(DocumentRelease, DelayedFinalReferenceDropIsDrained) {
    auto docModel = makeModel();
    ASSERT_TRUE(docModel);
    model::retireDocument(docModel);
    std::thread delayedDrop([model = std::move(docModel)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        model.reset();
    });

    EXPECT_FALSE(model::waitForDocumentRelease(std::chrono::milliseconds(30)));
    delayedDrop.join();
    EXPECT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
}
