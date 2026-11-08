#include "../core/TempDir.h"
#include "core/CancelToken.h"
#include "core/LoadCoordinator.h"
#include "core/LoadSession.h"
#include "core/OcctLoadContract.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "io/ProgressStreamBuf.h"
#include "occtsupport/OcctBoundary.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Writer.hxx>
#include <NCollection_Sequence.hxx>
#include <STEPControl_Writer.hxx>
#include <TDF_Label.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <class F> void requireFinishes(std::chrono::milliseconds budget, const char* name, F&& body) {
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([finished, body = std::forward<F>(body)]() mutable {
        body();
        finished->store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!finished->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!finished->load(std::memory_order_acquire)) {
        worker.detach();
        std::fprintf(stderr, "DEADLOCK %s exceeded %lld ms\n", name, static_cast<long long>(budget.count()));
        std::_Exit(2);
    }
    worker.join();
}

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::filesystem::path utf8Path(std::u8string_view text) {
    return std::filesystem::path(std::u8string(text));
}

int documentCount() {
    std::lock_guard lock(steplens::xcafDocumentMutex());
    return XCAFApp_Application::GetApplication()->NbDocuments();
}

class DocumentCloser {
public:
    explicit DocumentCloser(opencascade::handle<TDocStd_Document> doc) : m_doc(std::move(doc)) {}

    ~DocumentCloser() {
        if (m_doc.IsNull()) {
            return;
        }
        try {
            std::lock_guard lock(steplens::xcafDocumentMutex());
            if (!m_doc.IsNull()) {
                XCAFApp_Application::GetApplication()->Close(m_doc);
            }
        } catch (...) {
        }
        m_doc.Nullify();
    }

    DocumentCloser(const DocumentCloser&) = delete;
    DocumentCloser& operator=(const DocumentCloser&) = delete;

private:
    opencascade::handle<TDocStd_Document> m_doc;
};

int freeShapeCount(const opencascade::handle<TDocStd_Document>& doc) {
    const auto tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    EXPECT_FALSE(tool.IsNull());
    if (tool.IsNull()) {
        return 0;
    }
    NCollection_Sequence<TDF_Label> shapes;
    tool->GetFreeShapes(shapes);
    return shapes.Upper();
}

bool writeBoxStep(const std::filesystem::path& path) {
    STEPControl_Writer writer;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    if (writer.Transfer(box, STEPControl_AsIs) != IFSelect_RetDone) {
        return false;
    }
    return writer.Write(pathUtf8(path).c_str()) == IFSelect_RetDone;
}

bool writeBoxIges(const std::filesystem::path& path) {
    IGESControl_Writer writer;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    if (!writer.AddShape(box)) {
        return false;
    }
    return static_cast<bool>(writer.Write(pathUtf8(path).c_str(), false));
}

struct SessionLoad {
    steplens::Result<steplens::io::LoadedDocument> result{
        steplens::Error{steplens::ErrorCode::Internal, "not run", "未运行"}};
    std::shared_ptr<steplens::LoadSession> session;
    bool regressed = false;
    int armCount = 0;
};

SessionLoad runLoad(const std::filesystem::path& file, bool cancelBeforeLoad) {
    SessionLoad out;
    const auto bytes = static_cast<std::uint64_t>(std::filesystem::file_size(file));
    steplens::LoadCoordinator coordinator;
    std::mutex mutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool release = !cancelBeforeLoad;

    coordinator.submit(steplens::LoadCoordinator::LoadRequest{
        bytes, {}, [&](const std::shared_ptr<steplens::LoadSession>& session) {
            {
                std::lock_guard lock(mutex);
                out.session = session;
                ready = true;
            }
            readyCv.notify_all();
            if (cancelBeforeLoad) {
                std::unique_lock lock(mutex);
                readyCv.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
            }

            std::atomic<bool> done{false};
            std::atomic<bool> regressed{false};
            std::atomic<float> highest{0.f};
            std::thread sampler([&] {
                while (!done.load(std::memory_order_acquire)) {
                    const float now = session->progress().fraction();
                    float seen = highest.load(std::memory_order_relaxed);
                    if (now + 1.0e-6f < seen) {
                        regressed.store(true, std::memory_order_relaxed);
                    }
                    while (now > seen &&
                           !highest.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });

            auto loader = steplens::io::makeLoader(file);
            if (!loader) {
                out.result = steplens::Error{steplens::ErrorCode::Internal, "null loader", "内部错误"};
            } else {
                out.result = loader->load(file, {}, session->progress(), session->token());
            }
            out.armCount = steplens::occt::OcctBoundary::armCountOnThisThread();
            done.store(true, std::memory_order_release);
            sampler.join();
            out.regressed = regressed.load(std::memory_order_relaxed);
            session->noteBackgroundReleased();
        }});

    {
        std::unique_lock lock(mutex);
        readyCv.wait_for(lock, std::chrono::seconds(5), [&] { return ready; });
    }
    if (cancelBeforeLoad && out.session) {
        out.session->cancel();
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        readyCv.notify_all();
    }
    coordinator.waitUntilIdle();
    return out;
}

void expectRemoved(const std::filesystem::path& path) {
    std::error_code removed;
    EXPECT_TRUE(std::filesystem::remove(path, removed)) << removed.message();
    EXPECT_FALSE(removed);
}

} // namespace

TEST(ProgressStreamBuf, CancelBetweenChunksReturnsEof) {
    steplens::test::TempDir dir;
    const auto path = dir.path() / "stream.bin";
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output);
        const std::vector<char> chunk(steplens::io::ProgressStreamBuf::kChunkBytes, 'A');
        output.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        output.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    const auto bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path));

    steplens::ProgressSink sink;
    sink.reset(bytes);
    steplens::CancelSource source;
    steplens::io::ProgressStreamBuf buffer(path, bytes, sink, source.token());
    ASSERT_TRUE(buffer.isOpen());
    std::istream input(&buffer);
    std::vector<char> chunk(steplens::io::ProgressStreamBuf::kChunkBytes);
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    ASSERT_EQ(input.gcount(), static_cast<std::streamsize>(chunk.size()));
    EXPECT_GT(sink.fraction(), 0.f);
    EXPECT_LT(sink.fraction(), steplens::kReadingBandEnd);
    EXPECT_EQ(sink.phase(), steplens::ProgressPhase::ReadingBytes);

    source.requestStop();
    input.clear();
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    EXPECT_EQ(input.gcount(), 0);
    EXPECT_TRUE(buffer.wasCancelled());
    EXPECT_LT(sink.fraction(), 1.f);
    EXPECT_EQ(sink.phase(), steplens::ProgressPhase::ReadingBytes);
    expectRemoved(path);
}

TEST(CadLoader, StepBoxCompletesThroughLoadSessionAndReleasesReader) {
    steplens::test::TempDir dir;
    const auto path = dir.path() / "s01-box.STEP";
    ASSERT_TRUE(writeBoxStep(path));
    const int before = documentCount();

    SessionLoad loaded;
    requireFinishes(std::chrono::seconds(60), "step-box", [&] { loaded = runLoad(path, false); });

    ASSERT_TRUE(loaded.session);
    ASSERT_TRUE(loaded.result.ok()) << loaded.result.error().message;
    EXPECT_TRUE(loaded.session->backgroundReleased());
    EXPECT_FALSE(loaded.session->progress().cancelFrozen());
    EXPECT_GE(loaded.session->progress().fraction(), steplens::kTransferBandEnd);
    EXPECT_EQ(loaded.session->progress().phase(), steplens::ProgressPhase::Presenting);
    EXPECT_FALSE(loaded.regressed);
    EXPECT_GE(loaded.armCount, 1);
    EXPECT_EQ(loaded.result.value().fileBytes, static_cast<std::uint64_t>(std::filesystem::file_size(path)));
    EXPECT_GT(loaded.result.value().lengthUnitToMm, 0.0);
    EXPECT_FALSE(loaded.result.value().schema.empty());
    EXPECT_FALSE(loaded.result.value().doc.IsNull());
    EXPECT_GT(freeShapeCount(loaded.result.value().doc), 0);
    EXPECT_EQ(documentCount(), before + 1);

    DocumentCloser closer(loaded.result.value().doc);
    expectRemoved(path);
    EXPECT_EQ(documentCount(), before + 1);
}

TEST(CadLoader, ChineseStepAndIgesPathsLoad) {
    steplens::test::TempDir dir;
    const auto stepAscii = dir.path() / "box.stp";
    const auto igesAscii = dir.path() / "box.igs";
    ASSERT_TRUE(writeBoxStep(stepAscii));
    ASSERT_TRUE(writeBoxIges(igesAscii));

    const auto chineseDir = dir.path() / utf8Path(u8"中文样例");
    std::filesystem::create_directories(chineseDir);
    const auto chineseStep = chineseDir / utf8Path(u8"螺钉.stp");
    const auto chineseIges = chineseDir / utf8Path(u8"轴承.igs");
    std::filesystem::copy_file(stepAscii, chineseStep);
    std::filesystem::copy_file(igesAscii, chineseIges);

    SessionLoad stepLoad;
    SessionLoad igesLoad;
    requireFinishes(std::chrono::seconds(60), "chinese-paths", [&] {
        stepLoad = runLoad(chineseStep, false);
        igesLoad = runLoad(chineseIges, false);
    });

    ASSERT_TRUE(stepLoad.result.ok()) << stepLoad.result.error().message;
    ASSERT_TRUE(igesLoad.result.ok()) << igesLoad.result.error().message;
    EXPECT_GT(freeShapeCount(stepLoad.result.value().doc), 0);
    EXPECT_GT(freeShapeCount(igesLoad.result.value().doc), 0);
    EXPECT_EQ(igesLoad.result.value().schema, "IGES");
    EXPECT_GE(stepLoad.armCount, 1);
    EXPECT_GE(igesLoad.armCount, 1);

    DocumentCloser stepDoc(stepLoad.result.value().doc);
    DocumentCloser igesDoc(igesLoad.result.value().doc);
    expectRemoved(chineseStep);
    expectRemoved(chineseIges);
}

TEST(CadLoader, CancelEndsSessionAndLaterLoadStillWorks) {
    steplens::test::TempDir dir;
    const auto path = dir.path() / "cancel-box.stp";
    ASSERT_TRUE(writeBoxStep(path));
    const int before = documentCount();

    SessionLoad cancelled;
    requireFinishes(std::chrono::seconds(60), "cancel-load", [&] { cancelled = runLoad(path, true); });

    ASSERT_TRUE(cancelled.session);
    ASSERT_FALSE(cancelled.result.ok());
    EXPECT_EQ(cancelled.result.error().code, steplens::ErrorCode::Cancelled);
    EXPECT_EQ(cancelled.result.error().userMessage, "已取消");
    EXPECT_TRUE(cancelled.session->backgroundReleased());
    EXPECT_TRUE(cancelled.session->progress().cancelFrozen());
    EXPECT_EQ(cancelled.session->progress().phase(), steplens::ProgressPhase::Cancelling);
    EXPECT_LT(cancelled.session->progress().fraction(), 1.f);
    EXPECT_EQ(documentCount(), before);

    SessionLoad again;
    requireFinishes(std::chrono::seconds(60), "load-after-cancel", [&] { again = runLoad(path, false); });
    ASSERT_TRUE(again.result.ok()) << again.result.error().message;
    EXPECT_TRUE(again.session->backgroundReleased());
    EXPECT_GT(freeShapeCount(again.result.value().doc), 0);
    EXPECT_EQ(documentCount(), before + 1);

    DocumentCloser closer(again.result.value().doc);
    expectRemoved(path);
}

TEST(CadLoader, FailureReportsChineseMessage) {
    steplens::test::TempDir dir;
    const auto missing = dir.path() / "missing.stp";
    const auto corruptStep = dir.path() / "corrupt.stp";
    const auto corruptIges = dir.path() / "corrupt.igs";
    const auto text = dir.path() / "notes.txt";
    {
        std::ofstream step(corruptStep, std::ios::binary);
        step << "this is not a step file\n";
        std::ofstream iges(corruptIges, std::ios::binary);
        iges << "this is not an iges file\n";
        std::ofstream plain(text, std::ios::binary);
        plain << "hello\n";
    }
    const int before = documentCount();

    auto loadDirect = [](const std::filesystem::path& path) {
        auto session = std::make_shared<steplens::LoadSession>(0);
        auto loader = steplens::io::makeLoader(path);
        EXPECT_TRUE(loader);
        if (!loader) {
            return steplens::Result<steplens::io::LoadedDocument>{
                steplens::Error{steplens::ErrorCode::Internal, "null loader", "内部错误"}};
        }
        return loader->load(path, {}, session->progress(), session->token());
    };

    const auto missingResult = loadDirect(missing);
    ASSERT_FALSE(missingResult.ok());
    EXPECT_EQ(missingResult.error().code, steplens::ErrorCode::FileNotFound);
    EXPECT_EQ(missingResult.error().userMessage, "找不到文件");

    const auto stepResult = loadDirect(corruptStep);
    ASSERT_FALSE(stepResult.ok());
    EXPECT_EQ(stepResult.error().code, steplens::ErrorCode::ParseFailed);
    EXPECT_EQ(stepResult.error().userMessage, "文件损坏或不是有效的 STEP 文件");

    const auto igesResult = loadDirect(corruptIges);
    ASSERT_FALSE(igesResult.ok());
    EXPECT_EQ(igesResult.error().code, steplens::ErrorCode::ParseFailed);
    EXPECT_EQ(igesResult.error().userMessage, "文件损坏或不是有效的 IGES 文件");

    const auto textResult = loadDirect(text);
    ASSERT_FALSE(textResult.ok());
    EXPECT_EQ(textResult.error().code, steplens::ErrorCode::UnsupportedFormat);
    EXPECT_EQ(textResult.error().userMessage, "不支持的文件格式");

    EXPECT_EQ(documentCount(), before);
}
