#include "../core/TempDir.h"
#include "core/OcctLoadContract.h"
#include "model/DocumentRelease.h"
#include "pipeline/LoadPipeline.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <XCAFApp_Application.hxx>

#ifdef STEPLENS_HAS_APP
#include "steplens_app_exe.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>

#if defined(STEPLENS_HAS_APP) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(STEPLENS_HAS_APP)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

int documentCount() {
    std::lock_guard lock(steplens::xcafDocumentMutex());
    return XCAFApp_Application::GetApplication()->NbDocuments();
}

bool writeBoxStep(const std::filesystem::path& path) {
    STEPControl_Writer writer;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    if (writer.Transfer(box, STEPControl_AsIs) != IFSelect_RetDone) {
        return false;
    }
    return writer.Write(pathUtf8(path).c_str()) == IFSelect_RetDone;
}

class RecordingHost final : public steplens::SceneHost {
public:
    explicit RecordingHost(std::thread::id guiThread) : m_gui(guiThread) {}

    void reset(std::shared_ptr<const steplens::model::DocumentModel> model) override {
        expectGui();
        ++m_resets;
        auto previous = std::move(m_model);
        m_model = std::move(model);
        steplens::model::retireDocument(std::move(previous));
    }

    void enqueue(steplens::mesh::MeshBatch batch) override {
        expectGui();
        ++m_enqueued;
        static_cast<void>(batch);
    }

    void pump() override {
        expectGui();
        ++m_pumps;
    }

    void clear() override {
        expectGui();
        ++m_clears;
        steplens::model::retireDocument(std::move(m_model));
    }

    void fitAll() override {
        expectGui();
        ++m_fits;
    }

    [[nodiscard]] int pending() const override {
        return m_holdDisplay ? 1 : 0;
    }

    void holdDisplay() {
        m_holdDisplay = true;
    }

    void releaseDisplay() {
        m_holdDisplay = false;
    }

    [[nodiscard]] bool holdsModel() const {
        return m_model != nullptr;
    }

    [[nodiscard]] int resets() const {
        return m_resets;
    }
    [[nodiscard]] int enqueued() const {
        return m_enqueued;
    }
    [[nodiscard]] int pumps() const {
        return m_pumps;
    }
    [[nodiscard]] int clears() const {
        return m_clears;
    }
    [[nodiscard]] int fits() const {
        return m_fits;
    }

    void dropModel() {
        m_model.reset();
    }

private:
    void expectGui() const {
        EXPECT_EQ(std::this_thread::get_id(), m_gui);
    }

    std::thread::id m_gui;
    std::shared_ptr<const steplens::model::DocumentModel> m_model;
    bool m_holdDisplay = false;
    int m_resets = 0;
    int m_enqueued = 0;
    int m_pumps = 0;
    int m_clears = 0;
    int m_fits = 0;
};

bool driveUntil(steplens::LoadPipeline& pipeline,
                const std::function<bool(const steplens::LoadPipeline::Snapshot&)>& done,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    float highest = 0.f;
    while (std::chrono::steady_clock::now() < deadline) {
        pipeline.tick();
        const auto shot = pipeline.snapshot();
        if (shot.fraction + 1.0e-6f < highest) {
            ADD_FAILURE() << "progress went backwards";
            return false;
        }
        if (shot.fraction > highest) {
            highest = shot.fraction;
        }
        if (done(shot)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

struct ReleaseProbeGuard {
    ~ReleaseProbeGuard() {
        steplens::model::setDocumentReleaseProbe({});
        static_cast<void>(steplens::model::waitForDocumentRelease(std::chrono::seconds(30)));
    }
};

} // namespace

TEST(LoadPipeline, CancelThenOpenAgainReleasesDocuments) {
    ASSERT_TRUE(steplens::model::waitForDocumentRelease(std::chrono::seconds(30)));
    steplens::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "box.step";
    ASSERT_TRUE(writeBoxStep(file));
    const int baseline = documentCount();

    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);
    std::atomic<bool> releaseProbe{false};
    pipeline.setBeforePublishForTest([&](const steplens::CancelToken& token) {
        while (!releaseProbe.load(std::memory_order_acquire) && !token.isCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    pipeline.open(file);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pipeline.cancel();
    releaseProbe.store(true, std::memory_order_release);
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    pipeline.tick();
    const auto afterCancel = pipeline.snapshot();
    EXPECT_TRUE(afterCancel.activity == steplens::LoadActivity::Idle ||
                afterCancel.activity == steplens::LoadActivity::Ready);
    EXPECT_EQ(host.clears(), 0);
    EXPECT_EQ(documentCount(), baseline);

    pipeline.setBeforePublishForTest({});
    pipeline.open(file);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
    EXPECT_GE(host.resets(), 1);
    EXPECT_GE(host.enqueued(), 1);
    EXPECT_GE(host.pumps(), 1);
    EXPECT_GE(host.fits(), 1);

    std::thread::id releaseThread;
    std::atomic<bool> sawRelease{false};
    ReleaseProbeGuard probeGuard;
    steplens::model::setDocumentReleaseProbe([&](std::thread::id id) {
        releaseThread = id;
        sawRelease.store(true, std::memory_order_release);
    });
    pipeline.open(file);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    ASSERT_TRUE(sawRelease.load(std::memory_order_acquire));
    EXPECT_NE(releaseThread, std::this_thread::get_id());
    static_cast<void>(probeGuard);
}

TEST(LoadPipeline, CancelMidLoadReturnsToIdleCube) {
    ASSERT_TRUE(steplens::model::waitForDocumentRelease(std::chrono::seconds(30)));
    steplens::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "box.step";
    ASSERT_TRUE(writeBoxStep(file));
    const int baseline = documentCount();

    RecordingHost host(std::this_thread::get_id());
    host.holdDisplay();
    steplens::LoadPipeline pipeline(host);
    pipeline.open(file);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [&](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Loading && host.resets() >= 1;
        },
        std::chrono::seconds(60)));
    EXPECT_TRUE(host.holdsModel());

    std::atomic<bool> hold{true};
    std::thread::id releaseThread;
    ReleaseProbeGuard probeGuard;
    steplens::model::setDocumentReleaseProbe([&](std::thread::id id) {
        releaseThread = id;
        while (hold.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    pipeline.cancel();
    pipeline.tick();
    EXPECT_EQ(pipeline.snapshot().activity, steplens::LoadActivity::Releasing);
    EXPECT_GE(host.clears(), 1);
    EXPECT_FALSE(host.holdsModel());
    EXPECT_GT(steplens::model::documentsPendingRelease(), 0);
    hold.store(false, std::memory_order_release);
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    pipeline.tick();
    EXPECT_EQ(pipeline.snapshot().activity, steplens::LoadActivity::Idle);
    EXPECT_NE(releaseThread, std::this_thread::get_id());
    EXPECT_EQ(documentCount(), baseline);
    static_cast<void>(probeGuard);
}

TEST(LoadPipeline, EscapeAfterReadyKeepsModel) {
    ASSERT_TRUE(steplens::model::waitForDocumentRelease(std::chrono::seconds(30)));
    steplens::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "box.step";
    ASSERT_TRUE(writeBoxStep(file));

    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);
    pipeline.open(file);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
    EXPECT_TRUE(host.holdsModel());
    const int clears = host.clears();
    const int resets = host.resets();

    pipeline.cancel();
    pipeline.tick();
    EXPECT_EQ(pipeline.snapshot().activity, steplens::LoadActivity::Ready);
    EXPECT_EQ(host.clears(), clears);
    EXPECT_EQ(host.resets(), resets);
    EXPECT_TRUE(host.holdsModel());
    EXPECT_EQ(steplens::model::documentsPendingRelease(), 0);
}

TEST(LoadPipeline, OpenWhileReleaseInProgress) {
    steplens::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "box.step";
    ASSERT_TRUE(writeBoxStep(file));

    RecordingHost host(std::this_thread::get_id());
    host.holdDisplay();
    steplens::LoadPipeline pipeline(host);
    pipeline.open(file);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [&](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Loading && host.resets() >= 1;
        },
        std::chrono::seconds(60)));

    std::atomic<bool> hold{true};
    std::thread::id releaseThread;
    ReleaseProbeGuard probeGuard;
    steplens::model::setDocumentReleaseProbe([&](std::thread::id id) {
        releaseThread = id;
        while (hold.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    pipeline.cancel();
    pipeline.tick();
    ASSERT_EQ(pipeline.snapshot().activity, steplens::LoadActivity::Releasing);
    host.releaseDisplay();
    pipeline.open(file);
    hold.store(false, std::memory_order_release);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    EXPECT_NE(releaseThread, std::this_thread::get_id());
    EXPECT_GE(host.resets(), 2);
    static_cast<void>(probeGuard);
}

TEST(LoadPipeline, FailureReleasesOffGuiThreadAndCanReopen) {
    steplens::test::TempDir dir;
    const std::filesystem::path good = dir.path() / "box.step";
    const std::filesystem::path missing = dir.path() / "missing.step";
    const std::filesystem::path corrupt = dir.path() / "corrupt.step";
    ASSERT_TRUE(writeBoxStep(good));
    {
        std::ofstream output(corrupt, std::ios::binary);
        output << "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n";
    }

    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);

    pipeline.open(missing);
    pipeline.tick();
    const auto missingFailure = pipeline.takeFailure();
    ASSERT_TRUE(missingFailure.has_value());
    EXPECT_EQ(missingFailure->code, steplens::ErrorCode::FileNotFound);

    std::thread::id releaseThread;
    std::atomic<int> releases{0};
    ReleaseProbeGuard probeGuard;
    steplens::model::setDocumentReleaseProbe([&](std::thread::id id) {
        releaseThread = id;
        releases.fetch_add(1, std::memory_order_relaxed);
    });

    pipeline.open(corrupt);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity != steplens::LoadActivity::Loading &&
                   shot.activity != steplens::LoadActivity::Releasing;
        },
        std::chrono::seconds(60)));
    const auto corruptFailure = pipeline.takeFailure();
    ASSERT_TRUE(corruptFailure.has_value());
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    if (releases.load(std::memory_order_relaxed) > 0) {
        EXPECT_NE(releaseThread, std::this_thread::get_id());
    }

    pipeline.setFailAfterPublishForTest(true);
    pipeline.open(good);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity != steplens::LoadActivity::Loading &&
                   shot.activity != steplens::LoadActivity::Releasing;
        },
        std::chrono::seconds(60)));
    const auto meshFailure = pipeline.takeFailure();
    ASSERT_TRUE(meshFailure.has_value());
    EXPECT_EQ(meshFailure->code, steplens::ErrorCode::MeshFailed);
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    ASSERT_GT(releases.load(std::memory_order_relaxed), 0);
    EXPECT_NE(releaseThread, std::this_thread::get_id());

    pipeline.setFailAfterPublishForTest(false);
    static_cast<void>(probeGuard);
    steplens::model::setDocumentReleaseProbe({});
    pipeline.open(good);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
}

TEST(LoadPipeline, OpenReplacesQueuedModelOffGuiThread) {
    ASSERT_TRUE(steplens::model::waitForDocumentRelease(std::chrono::seconds(30)));
    steplens::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "box.step";
    ASSERT_TRUE(writeBoxStep(file));

    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);
    pipeline.open(file);
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    EXPECT_EQ(host.resets(), 0);

    std::thread::id releaseThread;
    std::atomic<bool> sawRelease{false};
    ReleaseProbeGuard probeGuard;
    steplens::model::setDocumentReleaseProbe([&](std::thread::id id) {
        releaseThread = id;
        sawRelease.store(true, std::memory_order_release);
    });
    pipeline.open(file);
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    ASSERT_TRUE(sawRelease.load(std::memory_order_acquire));
    EXPECT_NE(releaseThread, std::this_thread::get_id());
    static_cast<void>(probeGuard);
}

#if defined(STEPLENS_HAS_APP)

int runAppUntilReady(const std::filesystem::path& file, std::chrono::milliseconds timeout) {
    const std::string path = pathUtf8(file);
#if defined(_WIN32)
    SetEnvironmentVariableW(L"QT_QPA_PLATFORM", L"offscreen");
    SetEnvironmentVariableW(L"STEPLENS_QUIT_WHEN_READY", L"1");
    const std::wstring exe = [] {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, kStepLensExe, -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(needed > 0 ? needed - 1 : 0), L'\0');
        if (needed > 1) {
            MultiByteToWideChar(CP_UTF8, 0, kStepLensExe, -1, wide.data(), needed);
        }
        return wide;
    }();
    const std::wstring argument = [&] {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(needed > 0 ? needed - 1 : 0), L'\0');
        if (needed > 1) {
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), needed);
        }
        return wide;
    }();
    std::wstring command = L"\"" + exe + L"\" \"" + argument + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                       &process) == 0) {
        return -1;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count()));
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 1);
        code = 124;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        setenv("QT_QPA_PLATFORM", "offscreen", 1);
        setenv("STEPLENS_QUIT_WHEN_READY", "1", 1);
        setenv("ASAN_OPTIONS", "halt_on_error=1:detect_leaks=0", 1);
        execl(kStepLensExe, kStepLensExe, path.c_str(), nullptr);
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            return 128 + WTERMSIG(status);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 124;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
}

TEST(AppExit, CloseAfterDisplayedModelExitsCleanly) {
    if (std::getenv("STEPLENS_APP_EXIT") == nullptr) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT=1 后再跑。托管 CI 的 ctest 不带显示库路径，由冒烟步骤拉起。";
    }
    steplens::test::TempDir dir;
    std::filesystem::path file = dir.path() / "box.step";
    std::chrono::milliseconds timeout{60000};
    if (const char* samples = std::getenv("STEPLENS_SAMPLES")) {
        const std::filesystem::path x02 = std::filesystem::path(samples) / "Scania-Engine-V8-XT-Turbo.step";
        if (std::filesystem::is_regular_file(x02)) {
            file = x02;
            timeout = std::chrono::minutes(10);
        }
    }
    if (file.filename() == "box.step") {
        ASSERT_TRUE(writeBoxStep(file));
    }
    const int code = runAppUntilReady(file, timeout);
    EXPECT_EQ(code, 0);
}

#endif

TEST(LoadPipeline, MissingFileDoesNotStartALoad) {
    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);
    pipeline.open(std::filesystem::path("missing-steplens-file.step"));
    pipeline.tick();
    const auto failure = pipeline.takeFailure();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code, steplens::ErrorCode::FileNotFound);
    EXPECT_FALSE(failure->userMessage.empty());
    EXPECT_TRUE(pipeline.isSettled());
    EXPECT_EQ(host.resets(), 0);
}
