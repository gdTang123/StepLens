#include "../core/TempDir.h"
#include "core/OcctLoadContract.h"
#include "pipeline/LoadPipeline.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <XCAFApp_Application.hxx>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>

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
        m_model = std::move(model);
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
        m_model.reset();
    }

    void fitAll() override {
        expectGui();
        ++m_fits;
    }

    [[nodiscard]] int pending() const override {
        return 0;
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

} // namespace

TEST(LoadPipeline, CancelThenOpenAgainReleasesDocuments) {
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
    EXPECT_EQ(documentCount(), baseline + 1);

    pipeline.open(file);
    ASSERT_TRUE(driveUntil(
        pipeline,
        [](const steplens::LoadPipeline::Snapshot& shot) {
            return shot.activity == steplens::LoadActivity::Ready;
        },
        std::chrono::seconds(60)));
    EXPECT_EQ(documentCount(), baseline + 1);
}

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
