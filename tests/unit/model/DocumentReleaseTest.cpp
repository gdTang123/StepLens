#include "../core/TempDir.h"
#include "core/CancelToken.h"
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
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
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
    std::vector<std::thread> workers;
    for (int thread = 0; thread < threadCount; ++thread) {
        workers.emplace_back([&, thread] {
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
