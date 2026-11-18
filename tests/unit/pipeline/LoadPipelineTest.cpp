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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

bool lifecycleLogMatchesSha(const std::string& log, std::string_view sha) {
    if (sha.empty() || sha == "unknown")
        return false;
    const std::string expected = std::string("event=lifecycle-open identity=") + std::string(sha) + " ";
    return log.find(expected) != std::string::npos;
}

TEST(AppExitLogBinding, RejectsPreexistingOldGreenLog) {
    ASSERT_FALSE(std::string_view{STEPLENS_GIT_SHA}.empty());
    ASSERT_STRNE(STEPLENS_GIT_SHA, "unknown");
    const std::string oldGreen = "1 tid=1 event=lifecycle-open identity=old-sha exit=0\n"
                                 "2 tid=1 event=main-normal-return identity=pending=0 exit=0\n"
                                 "3 tid=2 event=retirer-end identity=pending=0 live=0 inflight=0 exit=0\n";
    ASSERT_FALSE(lifecycleLogMatchesSha(oldGreen, STEPLENS_GIT_SHA));
}

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
// offscreen 只证明 NullSceneHost 分支。Viewer/tree 三路必须 STEPLENS_APP_EXIT_VIEWER=1
// 且取消 offscreen，隐藏窗口即可；不是像素拾取或帧对比夹具。
// AppExit 测试带 label app_exit，被 unit 预设排除。默认 ctest --preset unit 里的 SKIP
// 不能算 R3 成功。冒烟必须用 --test-dir 显式发现这些测试，且不得出现 SKIPPED。
//
// 产品 Windows CI（RelWithDebInfo StepLens.exe、QTDIR、vcpkg bin 入 PATH 之后）：
//   $env:QT_PLUGIN_PATH = Join-Path $env:QTDIR "plugins"
//   $vcpkgBin = Join-Path (Get-Location) "build\msvc\vcpkg_installed\x64-windows\bin"
//   $env:PATH = "$vcpkgBin;$(Join-Path $env:QTDIR 'bin');$env:PATH"
//   $lifeDir = Join-Path (Get-Location) "build\msvc\RelWithDebInfo\logs"
//   New-Item -ItemType Directory -Force -Path $lifeDir | Out-Null
//   $env:STEPLENS_APP_EXIT = "1"
//   $env:QT_QPA_PLATFORM = "offscreen"
//   ctest --test-dir build/msvc -C RelWithDebInfo --no-tests=error --output-on-failure `
//     -R
//     "^AppExit\.(CloseEventWhileBusyQuickExits|CloseAfterDisplayedModelExitsCleanly|NormalReturnAfterCloseCompletes)$"
//   Remove-Item Env:QT_QPA_PLATFORM
//   $env:STEPLENS_APP_EXIT_VIEWER = "1"
//   ctest --test-dir build/msvc -C RelWithDebInfo --no-tests=error --output-on-failure `
//     -R "^AppExit\.(ViewerTreeCloseEventWhileBusy|ViewerTreeQuitWhenReadyHeldClose|ViewerTreeNormalReturn)$"
// 每次子进程使用 lifecycle-<SHA>-<用例>-<nonce>.log；测试先建空文件并校验 lifecycle-open SHA。
// 每个产品进程硬超时 60s，合成 box.step；HeldClose 用 Close gate 因果控制。
// 本机无 MSVC 产品主程序，不能把合同测试当作这三路已经跑通。
// locApp 本体析构无法插桩；companion 只是上界。产品 main 不抢先 prime Application。

struct AppSpawnOptions {
    bool quitWhenReady = false;
    bool closeWhileBusy = false;
    bool waitReleaseBeforeExit = false;
    bool holdCloseUntilAfterWindow = false;
    bool offscreen = true;
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
    const auto restoreSha = envOrEmpty("STEPLENS_LIFECYCLE_SHA");
    const auto restoreHeld = envOrEmpty("STEPLENS_HOLD_CLOSE_UNTIL_AFTER_WINDOW");
    timeout = std::min(timeout, std::chrono::milliseconds{60000});
    setEnvValue("QT_QPA_PLATFORM", options.offscreen ? "offscreen" : std::string{});
    setEnvValue("STEPLENS_QUIT_WHEN_READY", options.quitWhenReady ? "1" : std::string{});
    setEnvValue("STEPLENS_CLOSE_WHILE_BUSY", options.closeWhileBusy ? "1" : std::string{});
    setEnvValue("STEPLENS_WAIT_RELEASE_BEFORE_EXIT", options.waitReleaseBeforeExit ? "1" : std::string{});
    setEnvValue("STEPLENS_RELEASE_DELAY_MS", {});
    setEnvValue("STEPLENS_HOLD_CLOSE_UNTIL_AFTER_WINDOW",
                options.holdCloseUntilAfterWindow ? "1" : std::string{});
    setEnvValue("STEPLENS_LIFECYCLE_SHA", STEPLENS_GIT_SHA);
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
    setEnvValue("STEPLENS_HOLD_CLOSE_UNTIL_AFTER_WINDOW", restoreHeld);
    setEnvValue("STEPLENS_LIFECYCLE_SHA", restoreSha);
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
        WaitForSingleObject(process.hProcess, INFINITE);
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
        setEnvValue("STEPLENS_HOLD_CLOSE_UNTIL_AFTER_WINDOW", restoreHeld);
        setEnvValue("STEPLENS_LIFECYCLE_SHA", restoreSha);
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
    setEnvValue("STEPLENS_HOLD_CLOSE_UNTIL_AFTER_WINDOW", restoreHeld);
    setEnvValue("STEPLENS_LIFECYCLE_SHA", restoreSha);
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
            pid_t reaped = -1;
            do {
                reaped = waitpid(pid, &status, 0);
            } while (reaped < 0 && errno == EINTR);
            if (reaped != pid && !(reaped < 0 && errno == ECHILD)) {
                return 125;
            }
            return 124;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
}

int eventIdentityPos(const std::string& log, std::string_view event, std::string_view identity) {
    const std::string key =
        std::string("event=") + std::string(event) + " identity=" + std::string(identity) + " ";
    const auto at = log.find(key);
    return at == std::string::npos ? -1 : static_cast<int>(at);
}

void expectLifecycleIdentity(const std::string& log) {
    EXPECT_TRUE(lifecycleLogMatchesSha(log, STEPLENS_GIT_SHA))
        << "生命周期日志缺少本次构建 SHA 或 SHA 不符：" << STEPLENS_GIT_SHA << "\n"
        << log;
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
    return writeBoxStep(file);
}

std::filesystem::path exeLogsDir() {
    const auto dir = std::filesystem::path(kStepLensExe).parent_path() / "logs";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir;
}

std::filesystem::path lifecycleLogFor(const char* name) {
    static std::atomic<unsigned long long> nonce{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string filename = std::string("lifecycle-") + STEPLENS_GIT_SHA + "-" + name + "-" +
                                 std::to_string(stamp) + "-" + std::to_string(nonce.fetch_add(1)) + ".log";
    const auto path = exeLogsDir() / filename;
    std::ofstream fresh(path, std::ios::binary | std::ios::trunc);
    return path;
}

int eventPos(const std::string& log, std::string_view event) {
    const std::string key = std::string("event=") + std::string(event);
    const auto at = log.find(key);
    return at == std::string::npos ? -1 : static_cast<int>(at);
}

int pendingLoggedAt(const std::string& log, std::string_view event) {
    const int at = eventPos(log, event);
    if (at < 0) {
        return -1;
    }
    const auto from = static_cast<std::size_t>(at);
    const auto lineEnd = log.find('\n', from);
    const auto ident = log.find("identity=pending=", from);
    if (ident == std::string::npos || (lineEnd != std::string::npos && ident > lineEnd)) {
        return -1;
    }
    const auto start = ident + 17;
    const auto end = log.find_first_not_of("0123456789", start);
    if (start == end || start >= log.size() || (end != std::string::npos && end == start)) {
        return -1;
    }
    return std::stoi(log.substr(start, (end == std::string::npos ? log.size() : end) - start));
}

void expectPendingAfterWindow(const std::string& log) {
    EXPECT_GT(pendingLoggedAt(log, "main-after-window"), 0)
        << "main-after-window 时 pending 应 >0，不能用延时猜拆完\n"
        << log;
}

void expectEvent(const std::string& log, std::string_view event) {
    EXPECT_GE(eventPos(log, event), 0) << "缺少 event=" << event << "\n" << log;
}

void expectNoEvent(const std::string& log, std::string_view event) {
    EXPECT_EQ(eventPos(log, event), -1) << "不应出现 event=" << event << "\n" << log;
}

void expectEventBefore(const std::string& log, std::string_view earlier, std::string_view later) {
    const int a = eventPos(log, earlier);
    const int b = eventPos(log, later);
    ASSERT_GE(a, 0) << "缺少 event=" << earlier << "\n" << log;
    ASSERT_GE(b, 0) << "缺少 event=" << later << "\n" << log;
    EXPECT_LT(a, b) << earlier << " 应早于 " << later << "\n" << log;
}

void expectExeExists() {
    ASSERT_TRUE(std::filesystem::is_regular_file(kStepLensExe)) << kStepLensExe;
}

// 能证明：producer 已静止，以及 retirer-end 早于本进程内 Log/mutex 析构。
// companion 只是 locApp 析构的上界，不是 locApp 析构体本身。
void expectNormalReturnStaticOrder(const std::string& life) {
    expectEventBefore(life, "main-normal-return", "retirer-stop");
    expectEventBefore(life, "retirer-stop", "retirer-end");
    EXPECT_NE(life.find("pending=0 live=0 inflight=0"), std::string::npos) << life;
    const int stop = eventPos(life, "retirer-stop");
    ASSERT_GE(stop, 0) << life;
    EXPECT_EQ(life.find("event=producer-notify", static_cast<std::size_t>(stop)), std::string::npos)
        << "retirer-stop 之后仍有 producer 通知\n"
        << life;
    expectEventBefore(life, "retirer-end", "log-state-dtor");
    expectEventBefore(life, "retirer-end", "xcaf-mutex-dtor");
    ASSERT_GE(eventPos(life, "xcaf-app-companion-dtor"), 0)
        << "没有 companion 记录，不能给出 locApp 析构上界\n"
        << life;
    expectEventBefore(life, "retirer-end", "xcaf-app-companion-dtor");
}

TEST(AppExit, CloseAfterDisplayedModelExitsCleanly) {
    if (envOrEmpty("STEPLENS_APP_EXIT").empty()) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT=1 后再跑。托管 CI 的 ctest 不带显示库路径，由冒烟步骤拉起。";
    }
    expectExeExists();
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto logPath = std::filesystem::path(kStepLensExe).parent_path() / "logs" / "steplens.log";
    const auto lifecyclePath = lifecycleLogFor("lifecycle-offscreen-quit-when-ready.log");
    const auto logBytes = std::filesystem::is_regular_file(logPath) ? std::filesystem::file_size(logPath) : 0;
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.holdCloseUntilAfterWindow = true;
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
    expectLifecycleIdentity(life);
    expectEvent(life, "mainwindow-destructor-begin");
    expectEventBefore(life, "mainwindow-destructor-begin", "mainwindow-destructor-end");
    expectEventBefore(life, "mainwindow-destructor-end", "mainwindow-members-begin");
    expectEventBefore(life, "mainwindow-members-begin", "mainwindow-members-end");
    expectEventBefore(life, "mainwindow-members-end", "main-after-window");
    expectEventBefore(life, "main-after-window", "main-quick-exit-pending");
    expectPendingAfterWindow(life);
    expectEvent(life, "main-quick-exit-pending");
    expectNoEvent(life, "mainwindow-closeEvent-busy");
    expectNoEvent(life, "main-normal-return");
    expectEvent(life, "offscreen-host-created");
    expectNoEvent(life, "viewer-created");
    expectNoEvent(life, "tree-created");
}

TEST(AppExit, CloseEventWhileBusyQuickExits) {
    if (envOrEmpty("STEPLENS_APP_EXIT").empty()) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT=1 后再跑。由产品 CI 冒烟步骤拉起真实 StepLens.exe。";
    }
    expectExeExists();
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = lifecycleLogFor("lifecycle-offscreen-close-while-busy.log");
    AppSpawnOptions options;
    options.closeWhileBusy = true;
    options.offscreen = true;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    expectLifecycleIdentity(life);
    expectEvent(life, "offscreen-host-created");
    expectEvent(life, "close-while-busy-request");
    expectEvent(life, "mainwindow-closeEvent-busy");
    expectEvent(life, "end-process-now");
    expectNoEvent(life, "viewer-created");
    expectNoEvent(life, "tree-created");
    expectNoEvent(life, "main-quick-exit-pending");
    expectNoEvent(life, "main-normal-return");
}

TEST(AppExit, NormalReturnAfterCloseCompletes) {
    if (envOrEmpty("STEPLENS_APP_EXIT").empty()) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT=1 后再跑。由产品 CI 冒烟步骤拉起真实 StepLens.exe。";
    }
    expectExeExists();
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = lifecycleLogFor("lifecycle-offscreen-normal-return.log");
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.waitReleaseBeforeExit = true;
    options.offscreen = true;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    expectLifecycleIdentity(life);
    expectEvent(life, "offscreen-host-created");
    expectNoEvent(life, "viewer-created");
    expectNoEvent(life, "tree-created");
    expectEvent(life, "mainwindow-destructor-begin");
    expectEvent(life, "main-after-window");
    expectEvent(life, "main-wait-release-done");
    expectNoEvent(life, "main-quick-exit-pending");
    expectNoEvent(life, "end-process-now");
    expectNormalReturnStaticOrder(life);
}

TEST(AppExit, ViewerTreeCloseEventWhileBusy) {
    if (envOrEmpty("STEPLENS_APP_EXIT_VIEWER").empty()) {
        GTEST_SKIP() << "设置 STEPLENS_APP_EXIT_VIEWER=1 后在非 offscreen 隐藏窗口上跑真实 closeEvent。";
    }
    expectExeExists();
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = lifecycleLogFor("lifecycle-viewer-close-while-busy.log");
    AppSpawnOptions options;
    options.closeWhileBusy = true;
    options.offscreen = false;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    expectLifecycleIdentity(life);
    expectEventBefore(life, "viewer-created", "mainwindow-closeEvent-busy");
    expectEventBefore(life, "tree-created", "mainwindow-closeEvent-busy");
    expectEvent(life, "close-while-busy-request");
    expectEvent(life, "end-process-now");
    expectNoEvent(life, "offscreen-host-created");
    expectNoEvent(life, "main-quick-exit-pending");
    expectNoEvent(life, "main-normal-return");
}

TEST(AppExit, ViewerTreeQuitWhenReadyHeldClose) {
    if (envOrEmpty("STEPLENS_APP_EXIT_VIEWER").empty()) {
        GTEST_SKIP()
            << "设置 STEPLENS_APP_EXIT_VIEWER=1 后在非 offscreen 隐藏窗口上跑，取证 Viewer/tree 拆除。";
    }
    expectExeExists();
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = lifecycleLogFor("lifecycle-viewer-held-close.log");
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.holdCloseUntilAfterWindow = true;
    options.offscreen = false;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    expectLifecycleIdentity(life);
    expectEvent(life, "viewer-created");
    expectEvent(life, "tree-created");
    expectEventBefore(life, "mainwindow-destructor-begin", "mainwindow-destructor-end");
    expectEventBefore(life, "mainwindow-destructor-end", "mainwindow-members-begin");
    expectEventBefore(life, "mainwindow-members-begin", "mainwindow-members-end");
    const int viewerDestroyed = eventIdentityPos(life, "qobject-child-destroyed", "viewerWidget");
    const int treeDestroyed = eventIdentityPos(life, "qobject-child-destroyed", "assemblyTreePanel");
    EXPECT_GT(viewerDestroyed, eventPos(life, "mainwindow-members-end")) << life;
    EXPECT_GT(treeDestroyed, eventPos(life, "mainwindow-members-end")) << life;
    EXPECT_LT(viewerDestroyed, eventPos(life, "main-after-window")) << life;
    EXPECT_LT(treeDestroyed, eventPos(life, "main-after-window")) << life;
    expectEventBefore(life, "mainwindow-members-end", "main-after-window");
    expectEventBefore(life, "main-after-window", "main-quick-exit-pending");
    expectPendingAfterWindow(life);
    expectEvent(life, "main-quick-exit-pending");
    expectNoEvent(life, "mainwindow-closeEvent-busy");
    expectNoEvent(life, "offscreen-host-created");
}

TEST(AppExit, ViewerTreeNormalReturn) {
    if (envOrEmpty("STEPLENS_APP_EXIT_VIEWER").empty()) {
        GTEST_SKIP()
            << "设置 STEPLENS_APP_EXIT_VIEWER=1 后在非 offscreen 隐藏窗口上跑，取证正常 return 与静态顺序。";
    }
    expectExeExists();
    steplens::test::TempDir dir;
    std::filesystem::path file;
    std::chrono::milliseconds timeout{60000};
    ASSERT_TRUE(writeOrReuseModel(dir, file, timeout));
    const auto lifecyclePath = lifecycleLogFor("lifecycle-viewer-normal.log");
    AppSpawnOptions options;
    options.quitWhenReady = true;
    options.waitReleaseBeforeExit = true;
    options.offscreen = false;
    options.lifecycleLog = lifecyclePath;
    const int code = runProductApp(file, timeout, options);
    EXPECT_EQ(code, 0);
    const std::string life = readFileIfExists(lifecyclePath);
    expectLifecycleIdentity(life);
    expectEvent(life, "viewer-created");
    expectEvent(life, "tree-created");
    expectEvent(life, "qobject-child-destroyed");
    EXPECT_GE(eventIdentityPos(life, "qobject-child-destroyed", "viewerWidget"), 0) << life;
    EXPECT_GE(eventIdentityPos(life, "qobject-child-destroyed", "assemblyTreePanel"), 0) << life;
    expectNoEvent(life, "offscreen-host-created");
    expectNoEvent(life, "main-quick-exit-pending");
    expectNoEvent(life, "end-process-now");
    expectNormalReturnStaticOrder(life);
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
