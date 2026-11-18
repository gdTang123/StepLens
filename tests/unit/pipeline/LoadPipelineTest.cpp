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
        const bool same = previous && model && previous.get() == model.get();
        m_model = std::move(model);
        if (same)
            steplens::model::handoffDocument(std::move(previous));
        else
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

    [[nodiscard]] std::shared_ptr<const steplens::model::DocumentModel> document() const {
        return m_model;
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

// 回收线程取出文档后、减少 pending 之前会调用这个函数。门关着时它不能把计数减下去。
std::atomic<bool> releaseGateOpen{true};

void waitUntilReleaseGateOpens() {
    while (!releaseGateOpen.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct ReleaseGateGuard {
    ReleaseGateGuard() {
        releaseGateOpen.store(false, std::memory_order_release);
        steplens::model::setDocumentReleaseGate(&waitUntilReleaseGateOpens);
    }
    ~ReleaseGateGuard() {
        releaseGateOpen.store(true, std::memory_order_release);
        steplens::model::setDocumentReleaseGate(nullptr);
    }
    ReleaseGateGuard(const ReleaseGateGuard&) = delete;
    ReleaseGateGuard& operator=(const ReleaseGateGuard&) = delete;
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

    std::thread::id releaseThread;
    ReleaseProbeGuard probeGuard;
    steplens::model::setDocumentReleaseProbe([&](std::thread::id id) { releaseThread = id; });
    // 先关上门再取消。后台线程减 pending 之前会停在门上，断言不靠析构跑多快。
    ReleaseGateGuard gateGuard;
    pipeline.cancel();
    pipeline.tick();
    EXPECT_EQ(pipeline.snapshot().activity, steplens::LoadActivity::Releasing);
    EXPECT_GE(host.clears(), 1);
    EXPECT_FALSE(host.holdsModel());
    EXPECT_GT(steplens::model::documentsPendingRelease(), 0);
    releaseGateOpen.store(true, std::memory_order_release);
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    pipeline.tick();
    EXPECT_EQ(pipeline.snapshot().activity, steplens::LoadActivity::Idle);
    EXPECT_NE(releaseThread, std::this_thread::get_id());
    EXPECT_EQ(documentCount(), baseline);
    static_cast<void>(probeGuard);
}

TEST(LoadPipeline, RetiredDocumentWaitsForExternalStrongReference) {
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
    auto retained = host.document();
    ASSERT_TRUE(retained);

    steplens::model::retireDocument(retained);
    host.clear();
    EXPECT_GT(steplens::model::documentsPendingRelease(), 0);
    EXPECT_FALSE(pipeline.waitUntilSettled(std::chrono::milliseconds(30)));

    retained.reset();
    EXPECT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
}

TEST(LoadPipeline, CancelNewLoadBeforeAdoptKeepsPreviousModel) {
    steplens::test::TempDir dir;
    const std::filesystem::path fileA = dir.path() / "a.step";
    const std::filesystem::path fileB = dir.path() / "b.step";
    ASSERT_TRUE(writeBoxStep(fileA));
    ASSERT_TRUE(writeBoxStep(fileB));

    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);
    pipeline.open(fileA);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
    const auto displayedA = host.document();
    ASSERT_NE(displayedA, nullptr);
    const int clears = host.clears();
    const int resets = host.resets();

    std::atomic<bool> probeEntered{false};
    std::atomic<bool> releaseProbe{false};
    pipeline.setBeforePublishForTest([&](const steplens::CancelToken& token) {
        probeEntered.store(true, std::memory_order_release);
        while (!releaseProbe.load(std::memory_order_acquire) && !token.isCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    pipeline.open(fileB);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!probeEntered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(probeEntered.load(std::memory_order_acquire));
    EXPECT_EQ(host.resets(), resets);

    pipeline.cancel();
    releaseProbe.store(true, std::memory_order_release);
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    pipeline.tick();
    EXPECT_EQ(host.clears(), clears);
    EXPECT_EQ(host.document(), displayedA);
    EXPECT_EQ(pipeline.snapshot().activity, steplens::LoadActivity::Ready);
}

TEST(LoadPipeline, CurrentModelMailboxIsNotAdoptedDisplay) {
    steplens::test::TempDir dir;
    const std::filesystem::path fileA = dir.path() / "a.step";
    const std::filesystem::path fileB = dir.path() / "b.step";
    ASSERT_TRUE(writeBoxStep(fileA));
    ASSERT_TRUE(writeBoxStep(fileB));

    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);
    pipeline.open(fileA);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
    const auto displayedA = host.document();
    ASSERT_NE(displayedA, nullptr);
    const int resets = host.resets();

    pipeline.open(fileB);
    std::shared_ptr<const steplens::model::DocumentModel> publishedB;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        publishedB = pipeline.currentModel();
        if (publishedB && publishedB != displayedA) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(publishedB, nullptr);
    EXPECT_NE(publishedB, displayedA);
    EXPECT_EQ(host.resets(), resets);
    EXPECT_EQ(host.document(), displayedA);
    EXPECT_NE(publishedB, host.document());

    publishedB.reset();
    pipeline.cancel();
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    pipeline.tick();
    EXPECT_EQ(host.document(), displayedA);
}

TEST(LoadPipeline, CancelAfterNewModelAdoptedClearsDisplay) {
    steplens::test::TempDir dir;
    const std::filesystem::path fileA = dir.path() / "a.step";
    const std::filesystem::path fileB = dir.path() / "b.step";
    ASSERT_TRUE(writeBoxStep(fileA));
    ASSERT_TRUE(writeBoxStep(fileB));

    RecordingHost host(std::this_thread::get_id());
    steplens::LoadPipeline pipeline(host);
    pipeline.open(fileA);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));

    host.holdDisplay();
    pipeline.open(fileB);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [&](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Loading && host.resets() >= 2;
        },
        std::chrono::seconds(60)));
    auto displayedB = host.document();
    ASSERT_NE(displayedB, nullptr);
    const int clears = host.clears();

    pipeline.cancel();
    EXPECT_EQ(host.clears(), clears + 1);
    EXPECT_FALSE(host.holdsModel());
    EXPECT_NE(host.document(), displayedB);
    displayedB.reset();
    ASSERT_TRUE(pipeline.waitUntilSettled(std::chrono::seconds(30)));
    ASSERT_TRUE(steplens::model::waitForDocumentRelease(std::chrono::seconds(30)));
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

std::string envOrEmpty(const char* name) {
#if defined(_WIN32)
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string value(static_cast<std::size_t>(needed), '\0');
    const DWORD wrote = GetEnvironmentVariableA(name, value.data(), needed);
    if (wrote == 0) {
        return {};
    }
    value.resize(wrote);
    return value;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

// 产品退出取证（必须是 StepLens.exe 的真实 main/MainWindow，硬超时子进程）。
// 不得用 QObject 仿真树、NullSceneHost 单元路径或 ReleaseQuickExitMain 冒充下列三路。
// QT_QPA_PLATFORM=offscreen 时主窗走 NullSceneHost、不创建 ViewerWidget/树，只证明该分支的
// closeEvent 与 main 快退/正常 return。Viewer/tree 拆除需取消 offscreen，用隐藏窗口跑
// STEPLENS_APP_EXIT_VIEWER=1 的用例；那不是像素拾取或帧对比夹具。
//
// 产品 Windows CI（在已有 RelWithDebInfo StepLens.exe、QTDIR、vcpkg bin 入 PATH 之后）：
//   $env:QT_PLUGIN_PATH = Join-Path $env:QTDIR "plugins"
//   $vcpkgBin = Join-Path (Get-Location) "build\msvc\vcpkg_installed\x64-windows\bin"
//   $env:PATH = "$vcpkgBin;$(Join-Path $env:QTDIR 'bin');$env:PATH"
//   $lifeDir = Join-Path (Get-Location) "build\msvc\RelWithDebInfo\logs"
//   New-Item -ItemType Directory -Force -Path $lifeDir | Out-Null
//   $env:STEPLENS_APP_EXIT = "1"
//   $env:QT_QPA_PLATFORM = "offscreen"
//   $env:STEPLENS_LIFECYCLE_LOG = Join-Path $lifeDir "lifecycle-offscreen.log"
//   ctest --preset unit -C RelWithDebInfo --no-tests=error --output-on-failure `
//     -R
//     "^AppExit\.(CloseEventWhileBusyQuickExits|CloseAfterDisplayedModelExitsCleanly|NormalReturnAfterCloseCompletes)$"
//   Get-Content $env:STEPLENS_LIFECYCLE_LOG
//   Remove-Item Env:QT_QPA_PLATFORM
//   $env:STEPLENS_APP_EXIT_VIEWER = "1"
//   $env:STEPLENS_LIFECYCLE_LOG = Join-Path $lifeDir "lifecycle-viewer.log"
//   ctest --preset unit -C RelWithDebInfo --no-tests=error --output-on-failure `
//     -R "^AppExit\.(ViewerTreeQuitWhenReadyHeldClose|ViewerTreeNormalReturn)$"
//   Get-Content $env:STEPLENS_LIFECYCLE_LOG
// 本机无 MSVC 产品主程序，不能把合同测试或独立 Viewer 探针当作这三路已经跑通。

struct AppSpawnOptions {
    bool quitWhenReady = false;
    bool closeWhileBusy = false;
    bool waitReleaseBeforeExit = false;
    bool offscreen = true;
    unsigned delayMs = 0;
    std::filesystem::path lifecycleLog;
};

void setEnvValue(const char* name, const std::string& value) {
#if defined(_WIN32)
    if (value.empty()) {
        SetEnvironmentVariableA(name, nullptr);
    } else {
        SetEnvironmentVariableA(name, value.c_str());
    }
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

int runProductApp(const std::filesystem::path& file, std::chrono::milliseconds timeout,
                  const AppSpawnOptions& options) {
    const auto restorePlatform = envOrEmpty("QT_QPA_PLATFORM");
    const auto restoreQuit = envOrEmpty("STEPLENS_QUIT_WHEN_READY");
    const auto restoreBusy = envOrEmpty("STEPLENS_CLOSE_WHILE_BUSY");
    const auto restoreWait = envOrEmpty("STEPLENS_WAIT_RELEASE_BEFORE_EXIT");
    const auto restoreDelay = envOrEmpty("STEPLENS_RELEASE_DELAY_MS");
    const auto restoreLife = envOrEmpty("STEPLENS_LIFECYCLE_LOG");
    setEnvValue("QT_QPA_PLATFORM", options.offscreen ? "offscreen" : std::string{});
    setEnvValue("STEPLENS_QUIT_WHEN_READY", options.quitWhenReady ? "1" : std::string{});
    setEnvValue("STEPLENS_CLOSE_WHILE_BUSY", options.closeWhileBusy ? "1" : std::string{});
    setEnvValue("STEPLENS_WAIT_RELEASE_BEFORE_EXIT", options.waitReleaseBeforeExit ? "1" : std::string{});
    setEnvValue("STEPLENS_RELEASE_DELAY_MS",
                options.delayMs == 0 ? std::string{} : std::to_string(options.delayMs));
    setEnvValue("STEPLENS_LIFECYCLE_LOG", options.lifecycleLog.string());
#if defined(_WIN32)
    const std::wstring exe = std::filesystem::path(kStepLensExe).wstring();
    const std::wstring argument = file.wstring();
    std::wstring command = L"\"" + exe + L"\" \"" + argument + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!options.offscreen) {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        nullptr, &startup, &process);
    setEnvValue("QT_QPA_PLATFORM", restorePlatform);
    setEnvValue("STEPLENS_QUIT_WHEN_READY", restoreQuit);
    setEnvValue("STEPLENS_CLOSE_WHILE_BUSY", restoreBusy);
    setEnvValue("STEPLENS_WAIT_RELEASE_BEFORE_EXIT", restoreWait);
    setEnvValue("STEPLENS_RELEASE_DELAY_MS", restoreDelay);
    setEnvValue("STEPLENS_LIFECYCLE_LOG", restoreLife);
    if (created == 0) {
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
    const std::string path = pathUtf8(file);
    const pid_t pid = fork();
    if (pid < 0) {
        setEnvValue("QT_QPA_PLATFORM", restorePlatform);
        setEnvValue("STEPLENS_QUIT_WHEN_READY", restoreQuit);
        setEnvValue("STEPLENS_CLOSE_WHILE_BUSY", restoreBusy);
        setEnvValue("STEPLENS_WAIT_RELEASE_BEFORE_EXIT", restoreWait);
        setEnvValue("STEPLENS_RELEASE_DELAY_MS", restoreDelay);
        setEnvValue("STEPLENS_LIFECYCLE_LOG", restoreLife);
        return -1;
    }
    if (pid == 0) {
        setenv("ASAN_OPTIONS", "halt_on_error=1:detect_leaks=0", 1);
        execl(kStepLensExe, kStepLensExe, path.c_str(), nullptr);
        _exit(127);
    }
    setEnvValue("QT_QPA_PLATFORM", restorePlatform);
    setEnvValue("STEPLENS_QUIT_WHEN_READY", restoreQuit);
    setEnvValue("STEPLENS_CLOSE_WHILE_BUSY", restoreBusy);
    setEnvValue("STEPLENS_WAIT_RELEASE_BEFORE_EXIT", restoreWait);
    setEnvValue("STEPLENS_RELEASE_DELAY_MS", restoreDelay);
    setEnvValue("STEPLENS_LIFECYCLE_LOG", restoreLife);
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

int runAppUntilReady(const std::filesystem::path& file, std::chrono::milliseconds timeout) {
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.delayMs = 2000;
    options.offscreen = true;
    return runProductApp(file, timeout, options);
}

std::string readFileIfExists(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeOrReuseModel(steplens::test::TempDir& dir, std::filesystem::path& file,
                       std::chrono::milliseconds& timeout) {
    file = dir.path() / "box.step";
    timeout = std::chrono::milliseconds{60000};
    const std::string samples = envOrEmpty("STEPLENS_SAMPLES");
    if (!samples.empty()) {
        const std::filesystem::path x02 = std::filesystem::path(samples) / "Scania-Engine-V8-XT-Turbo.step";
        if (std::filesystem::is_regular_file(x02)) {
            file = x02;
            timeout = std::chrono::minutes(10);
            return true;
        }
    }
    return writeBoxStep(file);
}

TEST(AppExit, CloseAfterDisplayedModelExitsCleanly) {
    if (envOrEmpty("STEPLENS_APP_EXIT").empty()) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT=1 后再跑。托管 CI 的 ctest 不带显示库路径，由冒烟步骤拉起。";
    }
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto logPath = std::filesystem::path(kStepLensExe).parent_path() / "logs" / "steplens.log";
    const auto lifecyclePath = dir.path() / "lifecycle-quit-when-ready.log";
    const auto logBytes = std::filesystem::is_regular_file(logPath) ? std::filesystem::file_size(logPath) : 0;
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.delayMs = 2000;
    options.offscreen = true;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    ASSERT_TRUE(std::filesystem::is_regular_file(logPath)) << logPath;
    std::ifstream log(logPath, std::ios::binary);
    ASSERT_TRUE(log);
    log.seekg(static_cast<std::streamoff>(logBytes));
    const std::string appended((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    EXPECT_NE(appended.find("退出时文档仍在后台关闭，结束进程"), std::string::npos) << appended;
    const std::string life = readFileIfExists(lifecyclePath);
    EXPECT_NE(life.find("mainwindow-destructor-begin"), std::string::npos) << life;
    EXPECT_NE(life.find("mainwindow-destructor-end"), std::string::npos) << life;
    EXPECT_NE(life.find("main-after-window"), std::string::npos) << life;
    EXPECT_NE(life.find("main-quick-exit-pending"), std::string::npos) << life;
    EXPECT_EQ(life.find("mainwindow-closeEvent-busy"), std::string::npos) << life;
    EXPECT_EQ(life.find("main-normal-return"), std::string::npos) << life;
}

TEST(AppExit, CloseEventWhileBusyQuickExits) {
    if (envOrEmpty("STEPLENS_APP_EXIT").empty()) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT=1 后再跑。由产品 CI 冒烟步骤拉起真实 StepLens.exe。";
    }
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = dir.path() / "lifecycle-close-while-busy.log";
    AppSpawnOptions options;
    options.closeWhileBusy = true;
    options.offscreen = true;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    EXPECT_NE(life.find("mainwindow-closeEvent-busy"), std::string::npos) << life;
    EXPECT_NE(life.find("end-process-now"), std::string::npos) << life;
    EXPECT_EQ(life.find("main-quick-exit-pending"), std::string::npos) << life;
    EXPECT_EQ(life.find("main-normal-return"), std::string::npos) << life;
}

TEST(AppExit, NormalReturnAfterCloseCompletes) {
    if (envOrEmpty("STEPLENS_APP_EXIT").empty()) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT=1 后再跑。由产品 CI 冒烟步骤拉起真实 StepLens.exe。";
    }
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = dir.path() / "lifecycle-normal-return.log";
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.waitReleaseBeforeExit = true;
    options.offscreen = true;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    EXPECT_NE(life.find("mainwindow-destructor-begin"), std::string::npos) << life;
    EXPECT_NE(life.find("main-after-window"), std::string::npos) << life;
    EXPECT_NE(life.find("main-wait-release-done"), std::string::npos) << life;
    EXPECT_NE(life.find("main-normal-return"), std::string::npos) << life;
    EXPECT_EQ(life.find("main-quick-exit-pending"), std::string::npos) << life;
    EXPECT_EQ(life.find("end-process-now"), std::string::npos) << life;
    // 正常 return 才会走到静态析构；生命周期通道不依赖 Log 单例。
    EXPECT_NE(life.find("retirer-stop"), std::string::npos) << life;
    EXPECT_NE(life.find("retirer-end"), std::string::npos) << life;
}

TEST(AppExit, ViewerTreeQuitWhenReadyHeldClose) {
    if (envOrEmpty("STEPLENS_APP_EXIT_VIEWER").empty()) {
        GTEST_SKIP()
            << "设置 STEPLENS_APP_EXIT_VIEWER=1 后在非 offscreen 隐藏窗口上跑，取证 Viewer/tree 拆除。";
    }
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = dir.path() / "lifecycle-viewer-held-close.log";
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.delayMs = 2000;
    options.offscreen = false;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    EXPECT_NE(life.find("mainwindow-destructor-begin"), std::string::npos) << life;
    EXPECT_NE(life.find("mainwindow-members-begin"), std::string::npos) << life;
    EXPECT_NE(life.find("mainwindow-members-end"), std::string::npos) << life;
    EXPECT_NE(life.find("viewerWidget"), std::string::npos) << life;
    EXPECT_NE(life.find("assemblyTreePanel"), std::string::npos) << life;
    EXPECT_NE(life.find("main-quick-exit-pending"), std::string::npos) << life;
    EXPECT_EQ(life.find("mainwindow-closeEvent-busy"), std::string::npos) << life;
}

TEST(AppExit, ViewerTreeNormalReturn) {
    if (envOrEmpty("STEPLENS_APP_EXIT_VIEWER").empty()) {
        GTEST_SKIP()
            << "设置 STEPLENS_APP_EXIT_VIEWER=1 后在非 offscreen 隐藏窗口上跑，取证正常 return 与静态顺序。";
    }
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = dir.path() / "lifecycle-viewer-normal.log";
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.waitReleaseBeforeExit = true;
    options.offscreen = false;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    EXPECT_NE(life.find("viewerWidget"), std::string::npos) << life;
    EXPECT_NE(life.find("assemblyTreePanel"), std::string::npos) << life;
    EXPECT_NE(life.find("main-normal-return"), std::string::npos) << life;
    EXPECT_NE(life.find("retirer-end"), std::string::npos) << life;
    EXPECT_NE(life.find("retirer-log"), std::string::npos) << life;
    EXPECT_NE(life.find("retirer-xcaf"), std::string::npos) << life;
    EXPECT_EQ(life.find("main-quick-exit-pending"), std::string::npos) << life;
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
