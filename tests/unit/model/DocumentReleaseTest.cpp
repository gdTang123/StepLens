#include "../core/TempDir.h"
#include "../core/TestChildReap.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
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
    const TestEnvValue childMark = readTestEnvironment("STEPLENS_R2_FIRST_WAIT_CHILD");
    if (childMark.present && !childMark.value.empty()) {
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
    const TestEnvValue childMark = readTestEnvironment("STEPLENS_R1_ENQUEUE_CHILD");
    if (childMark.present && !childMark.value.empty()) {
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
static_assert(std::is_same_v<_se_translator_function, steplens::occt::OcctBoundary::SehTranslatorForTesting>);
static_assert(
    std::is_convertible_v<decltype(&sehSentinel), steplens::occt::OcctBoundary::SehTranslatorForTesting>);
static_assert(std::is_convertible_v<void (*)(unsigned, struct ::_EXCEPTION_POINTERS*),
                                    steplens::occt::OcctBoundary::SehTranslatorForTesting>);
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
    const auto previous = steplens::occt::OcctBoundary::exchangeSehTranslatorForTesting(noop);
    snap.translator = previous;
    steplens::occt::OcctBoundary::exchangeSehTranslatorForTesting(previous);
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

void logFpSeh(const char* tag, const FpSehSnapshot& snap) {
#if defined(_WIN32)
    std::fprintf(stderr, "%s control=0x%08x mxcsr=0x%08x translator_is_sentinel=%d translator_is_null=%d\n",
                 tag, snap.control, snap.mxcsr, snap.translator == sehSentinel ? 1 : 0,
                 snap.translator == nullptr ? 1 : 0);
#else
    std::fprintf(stderr, "%s round=%d\n", tag, snap.round);
#endif
    std::fflush(stderr);
}

void forceNonDefaultFp() {
#if defined(_WIN32)
    unsigned int ignored = 0;
    _controlfp_s(&ignored, _RC_DOWN, _MCW_RC);
#if defined(_M_X64) || defined(__x86_64__)
    unsigned int mxcsr = _mm_getcsr();
    mxcsr |= kMxcsrDaz | kMxcsrFtz;
    mxcsr = (mxcsr & ~kMxcsrRc) | 0x2000u;
    mxcsr |= kMxcsrMask;
    mxcsr = (mxcsr & ~kMxcsrStatus) | 0x20u;
    _mm_setcsr(mxcsr);
#endif
    steplens::occt::OcctBoundary::exchangeSehTranslatorForTesting(sehSentinel);
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
    logFpSeh("first-entry-before", expected);
    if (!roundingStillDownward()) {
        return 6;
    }
    if (!model::waitForDocumentRelease(std::chrono::seconds(10))) {
        return 8;
    }
    if (steplens::occt::OcctBoundary::armCountOnThisThread() < 1) {
        return 9;
    }
    const auto after = captureFpSeh();
    logFpSeh("first-entry-after", after);
    if (!promisedFpEqual(after, expected)) {
        return 11;
    }
    static_cast<void>(mode);
    return 0;
}

void expectHangChildFailsClosed(const char* tag, const char* markEnv, const char* markValue,
                                const char* hangEnv, const char* filter,
                                std::chrono::milliseconds runBudget) {
#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    TestEnvRestore mark(markEnv, markValue);
    TestEnvRestore hang(hangEnv, "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module + "\" --gtest_filter=" + filter;
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    mark.restore();
    hang.restore();
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result =
        waitOrKillWindowsProcess(guard.info.hProcess, runBudget, std::chrono::milliseconds{2000});
    logTestChildReap(tag, static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.killed);
    EXPECT_EQ(result.killStatus, 1);
    EXPECT_TRUE(result.reaped) << "kill 后应在有限时间内回收";
    EXPECT_NE(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap((std::string(tag) + "-close").c_str(), static_cast<long>(process.dwProcessId), cleanup);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    TestEnvRestore mark(markEnv, markValue);
    TestEnvRestore hang(hangEnv, "1");
    const pid_t child = testFork();
    if (child != 0) {
        mark.restore();
        hang.restore();
    }
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(), (std::string("--gtest_filter=") + filter).c_str(), nullptr);
        _exit(127);
    }
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    PosixProcessGuard guard(child);
    const auto result = waitOrKillPidOwned(guard, runBudget, std::chrono::milliseconds{2000});
    logTestChildReap(tag, static_cast<long>(child), result);
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.killed);
    EXPECT_EQ(result.killStatus, 1);
    EXPECT_TRUE(result.reaped) << "kill 后应在有限时间内回收";
    EXPECT_NE(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap((std::string(tag) + "-close").c_str(), static_cast<long>(child), cleanup);
    EXPECT_FALSE(cleanup.ownedAtEntry) << "已消费后不得再持有 kill 权";
    EXPECT_FALSE(cleanup.killed) << "close 不得再杀已注销身份";
#endif
}

} // namespace

// 验证非 worker 的进程主线程首次入口会恢复调用者 FP 政策。
// 这不是 QApplication 线程，不能当作 GUI 首入已验证；GUI 首入在 DocumentReleaseGuiFirstEntryTest。
TEST(DocumentRelease, FirstMainThreadEntryRestoresCallerFpPolicy) {
    const TestEnvValue childMark = readTestEnvironment("STEPLENS_B2_FP_ENTRY");
    if (childMark.present && childMark.value == "main") {
        if (readTestEnvironment("STEPLENS_B2_HANG_BEFORE_ENTRY").present) {
            testChildHangForever();
        }
        const int code = runFirstEntryFpChild("main");
        if (readTestEnvironment("STEPLENS_B2_HANG_AFTER_ENTRY").present) {
            testChildHangForever();
        }
        std::exit(code);
    }

#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    TestEnvRestore mark("STEPLENS_B2_FP_ENTRY", "main");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module +
                          "\" --gtest_filter=DocumentRelease.FirstMainThreadEntryRestoresCallerFpPolicy";
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    mark.restore();
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result = waitOrKillWindowsProcess(guard.info.hProcess, std::chrono::milliseconds{15000},
                                                 std::chrono::milliseconds{2000});
    logTestChildReap("first-main", static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap("first-main-close", static_cast<long>(process.dwProcessId), cleanup);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    TestEnvRestore mark("STEPLENS_B2_FP_ENTRY", "main");
    const pid_t child = testFork();
    if (child != 0) {
        mark.restore();
    }
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.FirstMainThreadEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    PosixProcessGuard guard(child);
    const auto result =
        waitOrKillPidOwned(guard, std::chrono::milliseconds{15000}, std::chrono::milliseconds{2000});
    logTestChildReap("first-main", static_cast<long>(child), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap("first-main-close", static_cast<long>(child), cleanup);
    EXPECT_FALSE(cleanup.ownedAtEntry) << "已消费后不得再持有 kill 权";
    EXPECT_FALSE(cleanup.killed) << "close 不得再杀已注销身份";
#endif
}

// 普通 wait 线程首次入口。同样不是 QApplication/GUI 验证。
TEST(DocumentRelease, FirstOrdinaryWaitThreadEntryRestoresCallerFpPolicy) {
    const TestEnvValue childMark = readTestEnvironment("STEPLENS_B2_FP_ENTRY");
    if (childMark.present && childMark.value == "waiter") {
        if (readTestEnvironment("STEPLENS_B2_HANG_BEFORE_ENTRY").present) {
            testChildHangForever();
        }
        int result = 4;
        std::thread waiter([&] { result = runFirstEntryFpChild("waiter"); });
        waiter.join();
        if (readTestEnvironment("STEPLENS_B2_HANG_AFTER_ENTRY").present) {
            testChildHangForever();
        }
        std::exit(result);
    }

#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    TestEnvRestore mark("STEPLENS_B2_FP_ENTRY", "waiter");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command =
        std::string("\"") + module +
        "\" --gtest_filter=DocumentRelease.FirstOrdinaryWaitThreadEntryRestoresCallerFpPolicy";
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    mark.restore();
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result = waitOrKillWindowsProcess(guard.info.hProcess, std::chrono::milliseconds{15000},
                                                 std::chrono::milliseconds{2000});
    logTestChildReap("first-wait", static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap("first-wait-close", static_cast<long>(process.dwProcessId), cleanup);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    TestEnvRestore mark("STEPLENS_B2_FP_ENTRY", "waiter");
    const pid_t child = testFork();
    if (child != 0) {
        mark.restore();
    }
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.FirstOrdinaryWaitThreadEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    PosixProcessGuard guard(child);
    const auto result =
        waitOrKillPidOwned(guard, std::chrono::milliseconds{15000}, std::chrono::milliseconds{2000});
    logTestChildReap("first-wait", static_cast<long>(child), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap("first-wait-close", static_cast<long>(child), cleanup);
    EXPECT_FALSE(cleanup.ownedAtEntry) << "已消费后不得再持有 kill 权";
    EXPECT_FALSE(cleanup.killed) << "close 不得再杀已注销身份";
#endif
}

TEST(OcctBoundary, ScopedThreadSignalRestoresRoundingAfterException) {
    forceNonDefaultFp();
    const auto expected = captureFpSeh();
    logFpSeh("exception-before", expected);
    ASSERT_TRUE(roundingStillDownward());
    try {
        steplens::occt::OcctBoundary::ScopedThreadSignal primed;
        logFpSeh("exception-in-scope", captureFpSeh());
        throw 1;
    } catch (int) {
    }
    const auto afterEx = captureFpSeh();
    logFpSeh("exception-after", afterEx);
    EXPECT_TRUE(promisedFpEqual(afterEx, expected));
    EXPECT_FALSE(steplens::occt::OcctBoundary::scopedThreadSignalRestoreFailed());
}

TEST(OcctBoundary, ScopedThreadSignalNestingRestoresOuterCallerState) {
    forceNonDefaultFp();
    const auto caller = captureFpSeh();
    logFpSeh("nest-caller", caller);
    ASSERT_TRUE(roundingStillDownward());
    {
        steplens::occt::OcctBoundary::ScopedThreadSignal outer;
        const auto inOuter = captureFpSeh();
        logFpSeh("nest-in-outer", inOuter);
        {
            steplens::occt::OcctBoundary::ScopedThreadSignal inner;
            logFpSeh("nest-in-inner", captureFpSeh());
        }
        const auto afterInner = captureFpSeh();
        logFpSeh("nest-after-inner", afterInner);
        EXPECT_TRUE(promisedFpEqual(afterInner, inOuter)) << "内层结束不得提前恢复到调用者状态";
    }
    const auto afterOuter = captureFpSeh();
    logFpSeh("nest-after-outer", afterOuter);
    EXPECT_TRUE(promisedFpEqual(afterOuter, caller));
    EXPECT_TRUE(roundingStillDownward());
}

TEST(OcctBoundary, ScopedThreadSignalStateIsPerThread) {
    forceNonDefaultFp();
    const auto caller = captureFpSeh();
    logFpSeh("cross-caller-before", caller);
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
        logFpSeh("cross-worker-before", expected);
        {
            steplens::occt::OcctBoundary::ScopedThreadSignal primed;
            logFpSeh("cross-worker-in-scope", captureFpSeh());
        }
        const auto afterWorker = captureFpSeh();
        logFpSeh("cross-worker-after", afterWorker);
        otherOk = promisedFpEqual(afterWorker, expected);
#if !defined(_WIN32)
        otherRound = fegetround();
#else
        unsigned int control = 0;
        _controlfp_s(&control, 0, 0);
        otherRound = static_cast<int>(control & _MCW_RC);
#endif
    });
    worker.join();
    const auto callerAfter = captureFpSeh();
    logFpSeh("cross-caller-after", callerAfter);
    EXPECT_TRUE(otherOk);
    EXPECT_TRUE(promisedFpEqual(callerAfter, caller));
#if !defined(_WIN32)
    EXPECT_EQ(otherRound, FE_UPWARD);
#else
    EXPECT_EQ(otherRound, static_cast<int>(_RC_UP));
#endif
}

TEST(OcctBoundary, SaveFailureDoesNotArmOrStartRetirer) {
    const TestEnvValue childMark = readTestEnvironment("STEPLENS_B2_SAVE_FAIL");
    if (childMark.present && !childMark.value.empty()) {
        if (readTestEnvironment("STEPLENS_B2_HANG_BEFORE_ENTRY").present) {
            testChildHangForever();
        }
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
            if (readTestEnvironment("STEPLENS_B2_HANG_AFTER_ENTRY").present) {
                testChildHangForever();
            }
            std::exit(0);
        }
    }
#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    TestEnvRestore mark("STEPLENS_B2_SAVE_FAIL", "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command =
        std::string("\"") + module + "\" --gtest_filter=OcctBoundary.SaveFailureDoesNotArmOrStartRetirer";
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    mark.restore();
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result = waitOrKillWindowsProcess(guard.info.hProcess, std::chrono::milliseconds{15000},
                                                 std::chrono::milliseconds{2000});
    logTestChildReap("save-fail", static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap("save-fail-close", static_cast<long>(process.dwProcessId), cleanup);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    TestEnvRestore mark("STEPLENS_B2_SAVE_FAIL", "1");
    const pid_t child = testFork();
    if (child != 0) {
        mark.restore();
    }
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(), "--gtest_filter=OcctBoundary.SaveFailureDoesNotArmOrStartRetirer",
              nullptr);
        _exit(127);
    }
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    PosixProcessGuard guard(child);
    const auto result =
        waitOrKillPidOwned(guard, std::chrono::milliseconds{15000}, std::chrono::milliseconds{2000});
    logTestChildReap("save-fail", static_cast<long>(child), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap("save-fail-close", static_cast<long>(child), cleanup);
    EXPECT_FALSE(cleanup.ownedAtEntry) << "已消费后不得再持有 kill 权";
    EXPECT_FALSE(cleanup.killed) << "close 不得再杀已注销身份";
#endif
}

TEST(OcctBoundary, ArmHookExceptionRollsBackCallerState) {
    forceNonDefaultFp();
    const auto expected = captureFpSeh();
    logFpSeh("arm-hook-before", expected);
    steplens::occt::OcctBoundary::setScopedThreadSignalArmHookForTesting([] { throw 2; });
    try {
        steplens::occt::OcctBoundary::ScopedThreadSignal primed;
        FAIL() << "arm hook should throw";
    } catch (int value) {
        EXPECT_EQ(value, 2);
    }
    steplens::occt::OcctBoundary::setScopedThreadSignalArmHookForTesting(nullptr);
    const auto afterArmHook = captureFpSeh();
    logFpSeh("arm-hook-after", afterArmHook);
    EXPECT_TRUE(promisedFpEqual(afterArmHook, expected));
    EXPECT_FALSE(steplens::occt::OcctBoundary::scopedThreadSignalRestoreFailed());
}

TEST(OcctBoundary, WorkerArmRemainsWithoutScopedRestore) {
    forceNonDefaultFp();
    const auto before = captureFpSeh();
    logFpSeh("worker-arm-before", before);
    steplens::occt::OcctBoundary::armWorkerThread();
    const auto afterArm = captureFpSeh();
    logFpSeh("worker-arm-after", afterArm);
    EXPECT_GE(steplens::occt::OcctBoundary::armCountOnThisThread(), 1);
    // 永久 arm 不会自行回到调用者；若本平台 arm 改变了状态，该差异必须保持。
    EXPECT_TRUE(promisedFpEqual(captureFpSeh(), afterArm));
    if (!promisedFpEqual(afterArm, before)) {
        EXPECT_FALSE(promisedFpEqual(captureFpSeh(), before));
    }
}

TEST(DocumentRelease, HangBeforeFirstMainEntryFailsClosed) {
    expectHangChildFailsClosed(
        "hang-before-first-main", "STEPLENS_B2_FP_ENTRY", "main", "STEPLENS_B2_HANG_BEFORE_ENTRY",
        "DocumentRelease.FirstMainThreadEntryRestoresCallerFpPolicy", std::chrono::milliseconds{2000});
}

TEST(DocumentRelease, HangAfterFirstMainEntryCleanupFailsClosed) {
    expectHangChildFailsClosed(
        "hang-after-first-main", "STEPLENS_B2_FP_ENTRY", "main", "STEPLENS_B2_HANG_AFTER_ENTRY",
        "DocumentRelease.FirstMainThreadEntryRestoresCallerFpPolicy", std::chrono::milliseconds{15000});
}

TEST(TestChildReap, FirstWaitFailureIsNotExitZero) {
#if defined(_WIN32)
    const auto result =
        waitOrKillWindowsProcess(nullptr, std::chrono::milliseconds{100}, std::chrono::milliseconds{100});
    logTestChildReap("first-wait-fail", 0, result);
    EXPECT_TRUE(result.waitFailed || result.timedOut);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_FALSE(result.reaped && result.exitCode == 0);
#else
    const auto result =
        waitOrKillPid(static_cast<pid_t>(-1), std::chrono::milliseconds{100}, std::chrono::milliseconds{100});
    logTestChildReap("first-wait-fail", -1, result);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_FALSE(result.reaped && result.exitCode == 0);
#endif
}

struct TestChildReapHookReset {
    ~TestChildReapHookReset() {
        testUnconfirmedCleanupHook = nullptr;
        testCloseHook = nullptr;
#if defined(_WIN32)
        testTerminateProcessHook = nullptr;
        testWaitForSingleObjectHook = nullptr;
#else
        testWaitpidHook = nullptr;
        testKillHook = nullptr;
        testForkHook = nullptr;
        testForkFailRemaining = 0;
#endif
    }
};

#if !defined(_WIN32)
pid_t spawnDirectHangChild() {
    const pid_t child = ::fork();
    if (child == 0) {
        testChildHangForever();
    }
    return child;
}

TEST(TestChildReap, GuardDestructorReapsAfterEarlyReturn) {
    TestChildReapHookReset reset;
    TestChildReap seen{};
    testCloseHook = [&](const char*, long, const TestChildReap& result) { seen = result; };
    const pid_t child = spawnDirectHangChild();
    ASSERT_GT(child, 0) << "启动失败不得记为退出码 0";
    [](pid_t pid) {
        PosixProcessGuard guard(pid);
        return;
    }(child);
    logTestChildReap("dtor-early-return", static_cast<long>(child), seen);
    EXPECT_TRUE(seen.ownedAtEntry);
    EXPECT_TRUE(seen.killed);
    EXPECT_EQ(seen.killStatus, 1);
    EXPECT_TRUE(seen.reaped);
    EXPECT_NE(seen.exitCode, 0);
}

TEST(TestChildReap, GuardDestructorReapsAfterFatalAssert) {
    TestChildReapHookReset reset;
    TestChildReap seen{};
    testCloseHook = [&](const char*, long, const TestChildReap& result) { seen = result; };
    static pid_t fatalChild = -1;
    fatalChild = spawnDirectHangChild();
    ASSERT_GT(fatalChild, 0) << "启动失败不得记为退出码 0";
    EXPECT_FATAL_FAILURE(
        {
            PosixProcessGuard guard(fatalChild);
            ASSERT_TRUE(false) << "injected fatal";
        },
        "injected fatal");
    logTestChildReap("dtor-fatal-assert", static_cast<long>(fatalChild), seen);
    EXPECT_TRUE(seen.ownedAtEntry);
    EXPECT_TRUE(seen.killed);
    EXPECT_EQ(seen.killStatus, 1);
    EXPECT_TRUE(seen.reaped);
    EXPECT_NE(seen.exitCode, 0);
}

TEST(TestChildReap, GuardDestructorReapsAfterException) {
    TestChildReapHookReset reset;
    TestChildReap seen{};
    testCloseHook = [&](const char*, long, const TestChildReap& result) { seen = result; };
    const pid_t child = spawnDirectHangChild();
    ASSERT_GT(child, 0) << "启动失败不得记为退出码 0";
    try {
        PosixProcessGuard guard(child);
        throw 1;
    } catch (int) {
    }
    logTestChildReap("dtor-exception", static_cast<long>(child), seen);
    EXPECT_TRUE(seen.ownedAtEntry);
    EXPECT_TRUE(seen.killed);
    EXPECT_EQ(seen.killStatus, 1);
    EXPECT_TRUE(seen.reaped);
    EXPECT_NE(seen.exitCode, 0);
}

TEST(TestChildReap, InjectedKillFailureIsUnconfirmedNotSuccess) {
    TestChildReapHookReset reset;
    bool unconfirmed = false;
    TestChildReap reported{};
    testUnconfirmedCleanupHook = [&](const char*, long, const TestChildReap& result) {
        unconfirmed = true;
        reported = result;
        return true;
    };
    testWaitpidHook = [](pid_t, int*, int) { return static_cast<pid_t>(0); };
    testKillHook = [](pid_t, int) {
        errno = EPERM;
        return -1;
    };
    PosixProcessGuard guard(static_cast<pid_t>(4242));
    const auto result = guard.close("inject-kill-fail");
    logTestChildReap("inject-kill-fail", 4242, result);
    EXPECT_TRUE(result.ownedAtEntry);
    EXPECT_TRUE(result.killed) << "killed 只表示尝试过";
    EXPECT_EQ(result.killStatus, 0);
    EXPECT_FALSE(result.reaped);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_TRUE(unconfirmed);
    EXPECT_FALSE(result.killStatus == 1 && result.killed) << "不能把尝试 kill 当成功";
}

TEST(TestChildReap, InjectedReapFailureIsUnconfirmedAfterKillAttempt) {
    TestChildReapHookReset reset;
    bool unconfirmed = false;
    testUnconfirmedCleanupHook = [&](const char*, long, const TestChildReap&) {
        unconfirmed = true;
        return true;
    };
    testWaitpidHook = [](pid_t, int*, int) { return static_cast<pid_t>(0); };
    testKillHook = [](pid_t, int) { return 0; };
    PosixProcessGuard guard(static_cast<pid_t>(4242));
    const auto result = guard.close("inject-reap-fail");
    logTestChildReap("inject-reap-fail", 4242, result);
    EXPECT_TRUE(result.killed);
    EXPECT_EQ(result.killStatus, 1);
    EXPECT_FALSE(result.reaped) << "尝试 kill 成功仍须确认回收";
    EXPECT_NE(result.exitCode, 0);
    EXPECT_TRUE(unconfirmed);
}

TEST(TestChildReap, CloseAfterConsumeDoesNotWaitOrKillReusedPid) {
    TestChildReapHookReset reset;
    const pid_t fake = static_cast<pid_t>(4242);
    int waitCalls = 0;
    int killCalls = 0;
    int phase = 0;
    testWaitpidHook = [&](pid_t pid, int* status, int) {
        EXPECT_EQ(pid, fake);
        ++waitCalls;
        if (phase == 0) {
            *status = 7 << 8;
            return pid;
        }
        ADD_FAILURE() << "已注销后不得再 waitpid";
        return pid;
    };
    testKillHook = [&](pid_t, int) {
        ++killCalls;
        ADD_FAILURE() << "已注销后不得再 kill";
        return 0;
    };
    PosixProcessGuard guard(fake);
    const auto first =
        waitOrKillPidOwned(guard, std::chrono::milliseconds{100}, std::chrono::milliseconds{100});
    logTestChildReap("consume-exit7", static_cast<long>(fake), first);
    EXPECT_TRUE(first.reaped);
    EXPECT_EQ(first.exitCode, 7);
    EXPECT_FALSE(guard.ownsKill());
    phase = 1;
    const auto cleanup = guard.close("consume-close");
    logTestChildReap("consume-close", static_cast<long>(fake), cleanup);
    EXPECT_FALSE(cleanup.ownedAtEntry);
    EXPECT_FALSE(cleanup.killed);
    EXPECT_EQ(killCalls, 0);
    EXPECT_EQ(waitCalls, 1);
    EXPECT_NE(first.exitCode, 0);
}

TEST(TestChildReap, DtorAfterConsumeDoesNotWaitOrKillReusedPid) {
    TestChildReapHookReset reset;
    const pid_t fake = static_cast<pid_t>(4243);
    int waitCalls = 0;
    int killCalls = 0;
    int phase = 0;
    testWaitpidHook = [&](pid_t pid, int* status, int) {
        EXPECT_EQ(pid, fake);
        ++waitCalls;
        if (phase == 0) {
            *status = 7 << 8;
            return pid;
        }
        ADD_FAILURE() << "析构不得再 waitpid 已注销身份";
        return pid;
    };
    testKillHook = [&](pid_t, int) {
        ++killCalls;
        ADD_FAILURE() << "析构不得再 kill 已注销身份";
        return 0;
    };
    {
        PosixProcessGuard guard(fake);
        const auto first =
            waitOrKillPidOwned(guard, std::chrono::milliseconds{100}, std::chrono::milliseconds{100});
        EXPECT_TRUE(first.reaped);
        EXPECT_EQ(first.exitCode, 7);
        EXPECT_FALSE(guard.ownsKill());
        phase = 1;
    }
    EXPECT_EQ(killCalls, 0);
    EXPECT_EQ(waitCalls, 1);
}
#endif

#if defined(_WIN32)
TEST(TestChildReap, InjectedWindowsReapFailureIsUnconfirmedNotSuccess) {
    TestChildReapHookReset reset;
    bool unconfirmed = false;
    testUnconfirmedCleanupHook = [&](const char*, long, const TestChildReap&) {
        unconfirmed = true;
        return true;
    };
    testWaitForSingleObjectHook = [](HANDLE, DWORD) { return WAIT_TIMEOUT; };
    testTerminateProcessHook = [](HANDLE, UINT) { return FALSE; };
    WindowsProcessGuard guard;
    PROCESS_INFORMATION info{};
    info.hProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1));
    info.hThread = nullptr;
    info.dwProcessId = 4242;
    guard.attach(info);
    const auto result = guard.close("inject-win-kill-fail");
    logTestChildReap("inject-win-kill-fail", 4242, result);
    EXPECT_TRUE(result.killed);
    EXPECT_EQ(result.killStatus, 0);
    EXPECT_FALSE(result.reaped);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_TRUE(unconfirmed);
    // close 仍应走到 CloseHandle；此处句柄为假值，目标实跑再核真实句柄路径
}
#endif

TEST(TestEnvRead, DistinguishesUnsetEmptyAndValue) {
    const char* name = "STEPLENS_TEST_ENV_READ_TRI";
    TestEnvRestore restore(name, "initial-value");

#if defined(_WIN32)
    ASSERT_TRUE(SetEnvironmentVariableA(name, nullptr));
#else
    ASSERT_EQ(unsetenv(name), 0);
#endif
    const TestEnvValue unset = readTestEnvironment(name);
    EXPECT_FALSE(unset.present);
    EXPECT_TRUE(unset.value.empty());

#if defined(_WIN32)
    ASSERT_TRUE(SetEnvironmentVariableA(name, ""));
#else
    ASSERT_EQ(setenv(name, "", 1), 0);
#endif
    const TestEnvValue empty = readTestEnvironment(name);
    EXPECT_TRUE(empty.present);
    EXPECT_TRUE(empty.value.empty());
    EXPECT_EQ(empty.value.c_str()[0], '\0');

#if defined(_WIN32)
    ASSERT_TRUE(SetEnvironmentVariableA(name, "read-value"));
#else
    ASSERT_EQ(setenv(name, "read-value", 1), 0);
#endif
    const TestEnvValue value = readTestEnvironment(name);
    EXPECT_TRUE(value.present);
    EXPECT_EQ(value.value, "read-value");
}

#if defined(_WIN32)
void expectWin32EnvUnset(const char* name) {
    char buf[8]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    const DWORD err = GetLastError();
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(err, static_cast<DWORD>(ERROR_ENVVAR_NOT_FOUND));
}

void expectWin32EnvPresentEmpty(const char* name) {
    char buf[8]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    const DWORD err = GetLastError();
    EXPECT_EQ(n, 0u);
    EXPECT_NE(err, static_cast<DWORD>(ERROR_ENVVAR_NOT_FOUND));
}

void expectWin32EnvPresentValue(const char* name, const char* expected) {
    char buf[1024]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    const DWORD err = GetLastError();
    EXPECT_NE(err, static_cast<DWORD>(ERROR_ENVVAR_NOT_FOUND));
    ASSERT_GT(n, 0u);
    ASSERT_LT(n, static_cast<DWORD>(sizeof(buf)));
    EXPECT_EQ(n, static_cast<DWORD>(std::strlen(expected)));
    EXPECT_STREQ(buf, expected);
}

TEST(TestEnvRestore, WindowsThreeStateExplicitRestore) {
    const char* name = "STEPLENS_TEST_ENV_RESTORE_TRI";
    SetEnvironmentVariableA(name, nullptr);
    expectWin32EnvUnset(name);
    {
        TestEnvRestore restore(name, "tmp-unset");
        restore.restore();
        expectWin32EnvUnset(name);
    }

    ASSERT_TRUE(SetEnvironmentVariableA(name, ""));
    expectWin32EnvPresentEmpty(name);
    {
        TestEnvRestore restore(name, "tmp-empty");
        restore.restore();
        expectWin32EnvPresentEmpty(name);
    }

    ASSERT_TRUE(SetEnvironmentVariableA(name, "origin-value"));
    expectWin32EnvPresentValue(name, "origin-value");
    {
        TestEnvRestore restore(name, "tmp-value");
        restore.restore();
        expectWin32EnvPresentValue(name, "origin-value");
    }

    const std::string longValue(400, 'L');
    ASSERT_TRUE(SetEnvironmentVariableA(name, longValue.c_str()));
    {
        TestEnvRestore restore(name, "tmp-long");
        restore.restore();
        bool present = false;
        std::string value;
        ASSERT_TRUE(readWin32Environment(name, &present, &value));
        EXPECT_TRUE(present);
        EXPECT_EQ(value, longValue);
        char probe[8]{};
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableA(name, probe, 8);
        EXPECT_GE(n, 8u);
        EXPECT_NE(GetLastError(), static_cast<DWORD>(ERROR_ENVVAR_NOT_FOUND));
    }
    SetEnvironmentVariableA(name, nullptr);
}

TEST(TestEnvRestore, WindowsThreeStateScopeEarlyReturn) {
    const char* name = "STEPLENS_TEST_ENV_RESTORE_SCOPE";
    SetEnvironmentVariableA(name, nullptr);
    [name] {
        TestEnvRestore restore(name, "tmp-unset");
        return;
    }();
    expectWin32EnvUnset(name);

    ASSERT_TRUE(SetEnvironmentVariableA(name, ""));
    [name] {
        TestEnvRestore restore(name, "tmp-empty");
        return;
    }();
    expectWin32EnvPresentEmpty(name);

    ASSERT_TRUE(SetEnvironmentVariableA(name, "kept-value"));
    [name] {
        TestEnvRestore restore(name, "tmp-value");
        return;
    }();
    expectWin32EnvPresentValue(name, "kept-value");
    SetEnvironmentVariableA(name, nullptr);
}
#endif
