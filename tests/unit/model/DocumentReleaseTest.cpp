#include "../core/TempDir.h"
#include "core/CancelToken.h"
#include "core/LifecycleTrace.h"
#include "core/Log.h"
#include "core/OcctLoadContract.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "io/XcafLifetime.h"
#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <atomic>
#include <cfenv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <eh.h>
#include <float.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
#endif

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
std::atomic<bool> dependencyBeforeLockEntered{false};
std::atomic<bool> dependencyAfterLockEntered{false};
std::atomic<bool> dependencyLockWaitWasSerialized{false};
std::atomic<bool> loadApplicationLocked{false};
std::atomic<bool> loadApplicationUnlocked{false};
std::atomic<int> waiterArmCount{-1};
std::thread::id dependencyWaiterId;
void dependencyBeforeLockHook() {
    dependencyBeforeLockEntered.store(true, std::memory_order_release);
}
void dependencyAfterLockHook() {
    dependencyLockWaitWasSerialized.store(loadApplicationUnlocked.load(std::memory_order_acquire),
                                          std::memory_order_release);
    waiterArmCount.store(steplens::occt::OcctBoundary::armCountOnThisThread(), std::memory_order_release);
    dependencyWaiterId = std::this_thread::get_id();
    dependencyAfterLockEntered.store(true, std::memory_order_release);
}
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

TEST(DocumentRelease, FirstWaitSerializesXcafApplicationInitializationInFreshProcess) {
    const char* childMark = std::getenv("STEPLENS_R2_FIRST_WAIT_CHILD");
    if (childMark != nullptr && *childMark != '\0') {
        std::atomic<bool> loadDone{false};
        std::atomic<bool> waiterResult{false};
        Handle(XCAFApp_Application) loadApplication;
        dependencyBeforeLockEntered.store(false, std::memory_order_release);
        dependencyAfterLockEntered.store(false, std::memory_order_release);
        dependencyLockWaitWasSerialized.store(false, std::memory_order_release);
        loadApplicationLocked.store(false, std::memory_order_release);
        loadApplicationUnlocked.store(false, std::memory_order_release);
        waiterArmCount.store(-1, std::memory_order_release);
        model::setDocumentReleaseDependencyHooksForTesting(dependencyBeforeLockHook, dependencyAfterLockHook);
        std::thread load([&] {
            steplens::occt::OcctBoundary::armWorkerThread();
            std::unique_lock lock(steplens::xcafDocumentMutex());
            loadApplicationLocked.store(true, std::memory_order_release);
            while (!dependencyBeforeLockEntered.load(std::memory_order_acquire))
                std::this_thread::yield();
            loadApplication = XCAFApp_Application::GetApplication();
            steplens::io::noteXcafApplicationAccessed();
            // 该标记紧邻显式解锁；等待者的持锁后钩只能在 mutex 实际释放后运行。
            loadApplicationUnlocked.store(true, std::memory_order_release);
            lock.unlock();
            loadDone.store(true, std::memory_order_release);
        });
        std::thread waiter([&] {
            while (!loadApplicationLocked.load(std::memory_order_acquire))
                std::this_thread::yield();
            waiterResult.store(model::waitForDocumentRelease(std::chrono::seconds(10)),
                               std::memory_order_release);
        });
        load.join();
        waiter.join();
        model::setDocumentReleaseDependencyHooksForTesting(nullptr, nullptr);
        if (!loadDone.load(std::memory_order_acquire) || !waiterResult.load(std::memory_order_acquire) ||
            !dependencyBeforeLockEntered.load(std::memory_order_acquire) ||
            !dependencyAfterLockEntered.load(std::memory_order_acquire) ||
            !loadApplicationUnlocked.load(std::memory_order_acquire) ||
            !dependencyLockWaitWasSerialized.load(std::memory_order_acquire) || waiterArmCount.load() < 1 ||
            dependencyWaiterId == model::documentReleaseWorkerThread()) {
            std::exit(4);
        }
        Handle(XCAFApp_Application) secondApplication = XCAFApp_Application::GetApplication();
        if (loadApplication.IsNull() || secondApplication != loadApplication)
            std::exit(5);
        return;
    }

#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    SetEnvironmentVariableA("STEPLENS_R2_FIRST_WAIT_CHILD", "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command =
        std::string("\"") + module +
        "\" --gtest_filter=DocumentRelease.FirstWaitSerializesXcafApplicationInitializationInFreshProcess";
    ASSERT_TRUE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process));
    SetEnvironmentVariableA("STEPLENS_R2_FIRST_WAIT_CHILD", nullptr);
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = 9;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 9);
        const DWORD reapResult = WaitForSingleObject(process.hProcess, 5000);
        GetExitCodeProcess(process.hProcess, &exitCode);
        EXPECT_EQ(reapResult, WAIT_OBJECT_0) << "TerminateProcess 后未能在有限时间内回收";
    } else if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    EXPECT_EQ(waitResult, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    setenv("STEPLENS_R2_FIRST_WAIT_CHILD", "1", 1);
    const pid_t child = fork();
    if (child < 0) {
        unsetenv("STEPLENS_R2_FIRST_WAIT_CHILD");
        FAIL() << "fork failed";
    }
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.FirstWaitSerializesXcafApplicationInitializationInFreshProcess",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    unsetenv("STEPLENS_R2_FIRST_WAIT_CHILD");
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(child, &status, WNOHANG) == child) {
            exited = true;
            break;
        }
        std::this_thread::yield();
    }
    if (!exited) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    ASSERT_TRUE(exited);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
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
    DWORD exitCode = 9;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 9);
        const DWORD reapResult = WaitForSingleObject(process.hProcess, 5000);
        GetExitCodeProcess(process.hProcess, &exitCode);
        EXPECT_EQ(reapResult, WAIT_OBJECT_0) << "TerminateProcess 后未能在有限时间内回收";
    }
    if (waitResult == WAIT_OBJECT_0)
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
        const DWORD reapResult = WaitForSingleObject(process.hProcess, 5000);
        GetExitCodeProcess(process.hProcess, &exitCode);
        EXPECT_EQ(reapResult, WAIT_OBJECT_0) << "TerminateProcess 后未能在有限时间内回收";
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

namespace {

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
constexpr unsigned kMxcsrStatus = 0x003Fu;
constexpr unsigned kMxcsrDaz = 0x0040u;
constexpr unsigned kMxcsrMask = 0x1F80u;
constexpr unsigned kMxcsrRc = 0x6000u;
constexpr unsigned kMxcsrFtz = 0x8000u;
constexpr unsigned kMxcsrPromised = kMxcsrStatus | kMxcsrDaz | kMxcsrMask | kMxcsrRc | kMxcsrFtz;
#endif

struct FpSehSnapshot {
#if defined(_WIN32)
    unsigned int control = 0;
    unsigned int mxcsr = 0;
    bool hasMxcsr = false;
    _se_translator_function translator = nullptr;
#else
    fenv_t env{};
    int round = 0;
#endif
};

#if defined(_WIN32)
void sehSentinel(unsigned, struct _EXCEPTION_POINTERS*) {}
#endif

FpSehSnapshot captureFpSeh() {
    FpSehSnapshot snap;
#if defined(_WIN32)
    _controlfp_s(&snap.control, 0, 0);
#if defined(_M_X64) || defined(__x86_64__)
    snap.mxcsr = _mm_getcsr();
    snap.hasMxcsr = true;
#endif
    auto noop = [](unsigned, struct _EXCEPTION_POINTERS*) {};
    const auto previous = _set_se_translator(noop);
    snap.translator = previous;
    _set_se_translator(previous);
#else
    snap.round = fegetround();
    static_cast<void>(fegetenv(&snap.env));
#endif
    return snap;
}

bool promisedFpEqual(const FpSehSnapshot& a, const FpSehSnapshot& b) {
#if defined(_WIN32)
    const unsigned legal = _MCW_EM | _MCW_RC | _MCW_DN;
    if ((a.control & legal) != (b.control & legal)) {
        return false;
    }
    if (a.hasMxcsr != b.hasMxcsr) {
        return false;
    }
#if defined(_M_X64) || defined(__x86_64__)
    if (a.hasMxcsr && (a.mxcsr & kMxcsrPromised) != (b.mxcsr & kMxcsrPromised)) {
        return false;
    }
#endif
    return a.translator == b.translator;
#else
    return a.round == b.round;
#endif
}

void forceNonDefaultFp() {
#if defined(_WIN32)
    unsigned int ignored = 0;
    _controlfp_s(&ignored, _RC_DOWN, _MCW_RC);
#if defined(_M_X64) || defined(__x86_64__)
    unsigned int mxcsr = _mm_getcsr();
    mxcsr |= kMxcsrDaz | kMxcsrFtz;
    mxcsr = (mxcsr & ~kMxcsrRc) | 0x2000u;
    mxcsr &= ~kMxcsrStatus;
    _mm_setcsr(mxcsr);
#endif
    _set_se_translator(sehSentinel);
#else
    static_cast<void>(fesetround(FE_DOWNWARD));
    feclearexcept(FE_ALL_EXCEPT);
#endif
}

bool roundingStillDownward() {
#if defined(_WIN32)
    unsigned int control = 0;
    _controlfp_s(&control, 0, 0);
    return (control & _MCW_RC) == _RC_DOWN;
#else
    return fegetround() == FE_DOWNWARD;
#endif
}

int runFirstEntryFpChild(const char* mode) {
    forceNonDefaultFp();
    const auto expected = captureFpSeh();
    if (!roundingStillDownward()) {
        return 6;
    }
    if (!model::waitForDocumentRelease(std::chrono::seconds(10))) {
        return 8;
    }
    if (steplens::occt::OcctBoundary::armCountOnThisThread() < 1) {
        return 9;
    }
    if (!promisedFpEqual(captureFpSeh(), expected)) {
        return 11;
    }
    static_cast<void>(mode);
    return 0;
}

} // namespace

// 验证非 worker 的进程主线程首次入口会恢复调用者 FP 政策。
// 这不是 QApplication 线程，不能当作 GUI 首入已验证。GUI 首入仍待卫裁定与带 QApplication 的测试。
TEST(DocumentRelease, FirstMainThreadEntryRestoresCallerFpPolicy) {
    const char* childMark = std::getenv("STEPLENS_B2_FP_ENTRY");
    if (childMark != nullptr && std::string(childMark) == "main") {
        std::exit(runFirstEntryFpChild("main"));
    }

#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    SetEnvironmentVariableA("STEPLENS_B2_FP_ENTRY", "main");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module +
                          "\" --gtest_filter=DocumentRelease.FirstMainThreadEntryRestoresCallerFpPolicy";
    ASSERT_TRUE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process));
    SetEnvironmentVariableA("STEPLENS_B2_FP_ENTRY", nullptr);
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = 9;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 9);
        const DWORD reapResult = WaitForSingleObject(process.hProcess, 5000);
        GetExitCodeProcess(process.hProcess, &exitCode);
        EXPECT_EQ(reapResult, WAIT_OBJECT_0);
    } else if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    EXPECT_EQ(waitResult, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u) << "main first-entry FP/SEH restore failed, code=" << exitCode;
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    setenv("STEPLENS_B2_FP_ENTRY", "main", 1);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.FirstMainThreadEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    unsetenv("STEPLENS_B2_FP_ENTRY");
    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}

// 普通 wait 线程首次入口。同样不是 QApplication/GUI 验证。
TEST(DocumentRelease, FirstOrdinaryWaitThreadEntryRestoresCallerFpPolicy) {
    const char* childMark = std::getenv("STEPLENS_B2_FP_ENTRY");
    if (childMark != nullptr && std::string(childMark) == "waiter") {
        int result = 4;
        std::thread waiter([&] { result = runFirstEntryFpChild("waiter"); });
        waiter.join();
        std::exit(result);
    }

#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    SetEnvironmentVariableA("STEPLENS_B2_FP_ENTRY", "waiter");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command =
        std::string("\"") + module +
        "\" --gtest_filter=DocumentRelease.FirstOrdinaryWaitThreadEntryRestoresCallerFpPolicy";
    ASSERT_TRUE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process));
    SetEnvironmentVariableA("STEPLENS_B2_FP_ENTRY", nullptr);
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = 9;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 9);
        const DWORD reapResult = WaitForSingleObject(process.hProcess, 5000);
        GetExitCodeProcess(process.hProcess, &exitCode);
        EXPECT_EQ(reapResult, WAIT_OBJECT_0);
    } else if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    EXPECT_EQ(waitResult, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u) << "ordinary wait first-entry FP/SEH restore failed, code=" << exitCode;
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    setenv("STEPLENS_B2_FP_ENTRY", "waiter", 1);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.FirstOrdinaryWaitThreadEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    unsetenv("STEPLENS_B2_FP_ENTRY");
    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}

TEST(OcctBoundary, ScopedThreadSignalRestoresRoundingAfterException) {
    forceNonDefaultFp();
    const auto expected = captureFpSeh();
    ASSERT_TRUE(roundingStillDownward());
    try {
        steplens::occt::OcctBoundary::ScopedThreadSignal primed;
        throw 1;
    } catch (int) {
    }
    EXPECT_TRUE(promisedFpEqual(captureFpSeh(), expected));
    EXPECT_FALSE(steplens::occt::OcctBoundary::scopedThreadSignalRestoreFailed());
}

TEST(OcctBoundary, ScopedThreadSignalNestingRestoresOuterCallerState) {
    forceNonDefaultFp();
    const auto caller = captureFpSeh();
    ASSERT_TRUE(roundingStillDownward());
    {
        steplens::occt::OcctBoundary::ScopedThreadSignal outer;
        const auto inOuter = captureFpSeh();
        {
            steplens::occt::OcctBoundary::ScopedThreadSignal inner;
        }
        const auto afterInner = captureFpSeh();
        EXPECT_TRUE(promisedFpEqual(afterInner, inOuter)) << "内层结束不得提前恢复到调用者状态";
    }
    EXPECT_TRUE(promisedFpEqual(captureFpSeh(), caller));
    EXPECT_TRUE(roundingStillDownward());
}

TEST(OcctBoundary, ScopedThreadSignalStateIsPerThread) {
    forceNonDefaultFp();
    const auto caller = captureFpSeh();
    int otherRound = 0;
    bool otherOk = false;
    std::thread worker([&] {
#if !defined(_WIN32)
        static_cast<void>(fesetround(FE_UPWARD));
#else
        unsigned int ignored = 0;
        _controlfp_s(&ignored, _RC_UP, _MCW_RC);
#endif
        const auto expected = captureFpSeh();
        {
            steplens::occt::OcctBoundary::ScopedThreadSignal primed;
        }
        otherOk = promisedFpEqual(captureFpSeh(), expected);
#if !defined(_WIN32)
        otherRound = fegetround();
#else
        unsigned int control = 0;
        _controlfp_s(&control, 0, 0);
        otherRound = static_cast<int>(control & _MCW_RC);
#endif
    });
    worker.join();
    EXPECT_TRUE(otherOk);
    EXPECT_TRUE(promisedFpEqual(captureFpSeh(), caller));
#if !defined(_WIN32)
    EXPECT_EQ(otherRound, FE_UPWARD);
#else
    EXPECT_EQ(otherRound, static_cast<int>(_RC_UP));
#endif
}

TEST(OcctBoundary, SaveFailureDoesNotArmOrStartRetirer) {
    const char* childMark = std::getenv("STEPLENS_B2_SAVE_FAIL");
    if (childMark != nullptr && *childMark != '\0') {
        steplens::occt::OcctBoundary::setScopedThreadSignalSaveHookForTesting([] { return false; });
        try {
            static_cast<void>(model::waitForDocumentRelease(std::chrono::seconds(5)));
            std::exit(12);
        } catch (const std::exception&) {
            if (steplens::occt::OcctBoundary::armCountOnThisThread() != 0) {
                std::exit(13);
            }
            if (steplens::occt::OcctBoundary::scopedThreadSignalRestoreFailed()) {
                std::exit(14);
            }
            std::exit(0);
        }
    }
#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    SetEnvironmentVariableA("STEPLENS_B2_SAVE_FAIL", "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command =
        std::string("\"") + module + "\" --gtest_filter=OcctBoundary.SaveFailureDoesNotArmOrStartRetirer";
    ASSERT_TRUE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process));
    SetEnvironmentVariableA("STEPLENS_B2_SAVE_FAIL", nullptr);
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = 9;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    EXPECT_EQ(waitResult, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    setenv("STEPLENS_B2_SAVE_FAIL", "1", 1);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(), "--gtest_filter=OcctBoundary.SaveFailureDoesNotArmOrStartRetirer",
              nullptr);
        _exit(127);
    }
    unsetenv("STEPLENS_B2_SAVE_FAIL");
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}

TEST(OcctBoundary, ArmHookExceptionRollsBackCallerState) {
    forceNonDefaultFp();
    const auto expected = captureFpSeh();
    steplens::occt::OcctBoundary::setScopedThreadSignalArmHookForTesting([] { throw 2; });
    try {
        steplens::occt::OcctBoundary::ScopedThreadSignal primed;
        FAIL() << "arm hook should throw";
    } catch (int value) {
        EXPECT_EQ(value, 2);
    }
    steplens::occt::OcctBoundary::setScopedThreadSignalArmHookForTesting(nullptr);
    EXPECT_TRUE(promisedFpEqual(captureFpSeh(), expected));
    EXPECT_FALSE(steplens::occt::OcctBoundary::scopedThreadSignalRestoreFailed());
}

TEST(OcctBoundary, WorkerArmRemainsWithoutScopedRestore) {
    forceNonDefaultFp();
    const auto before = captureFpSeh();
    steplens::occt::OcctBoundary::armWorkerThread();
    const auto afterArm = captureFpSeh();
    EXPECT_GE(steplens::occt::OcctBoundary::armCountOnThisThread(), 1);
    // 永久 arm 不会自行回到调用者；若本平台 arm 改变了状态，该差异必须保持。
    EXPECT_TRUE(promisedFpEqual(captureFpSeh(), afterArm));
    if (!promisedFpEqual(afterArm, before)) {
        EXPECT_FALSE(promisedFpEqual(captureFpSeh(), before));
    }
}
