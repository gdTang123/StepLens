#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
// clang-format on
#endif

// 只在 windows-build 的独立步骤里设 STEPLENS_STEP_HEAP_REPRO=1。
// 普通 unit 预设不设这个变量，测试跳过，避免把「Configure, build, test」拖很长。
// 路径是 T16 打开后的产品路径：makeLoader、DocumentModel::build、网格化、
// retireDocument、waitForDocumentRelease、io::releaseFreedHeap。
// push_back 不加测试锁，只靠 Tessellator::run 串行化 onBatch。

#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "io/HeapRelease.h"
#include "io/ILoader.h"
#include "mesh/Tessellator.h"
#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
// clang-format on
#endif

namespace {

constexpr int kRounds = 10;

bool reproEnabled() {
#if defined(_WIN32)
    char buffer[8] = {};
    const DWORD length =
        GetEnvironmentVariableA("STEPLENS_STEP_HEAP_REPRO", buffer, static_cast<DWORD>(sizeof(buffer)));
    return length == 1 && buffer[0] == '1';
#else
    const char* value = std::getenv("STEPLENS_STEP_HEAP_REPRO");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
#endif
}

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void phase(std::string_view message) {
    std::cout << "step_heap_repro " << message << std::endl;
}

bool writeBoxGridStep(const std::filesystem::path& path, int boxCount) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    int side = 1;
    while (side * side < boxCount) {
        ++side;
    }
    for (int index = 0; index < boxCount; ++index) {
        const double x = static_cast<double>(index % side) * 15.0;
        const double y = static_cast<double>(index / side) * 15.0;
        builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(x, y, 0.0), 10.0, 10.0, 10.0).Shape());
    }
    STEPControl_Writer writer;
    if (writer.Transfer(compound, STEPControl_AsIs) != IFSelect_RetDone) {
        return false;
    }
    return writer.Write(pathUtf8(path).c_str()) == IFSelect_RetDone;
}

void loadMeshRetireReclaim(const std::filesystem::path& file, std::string_view tag, int round) {
    phase(std::format("file={} round={} phase=load_begin", tag, round));
    const auto bytes = std::filesystem::file_size(file);
    steplens::ProgressSink sink;
    sink.reset(static_cast<std::uint64_t>(bytes));
    auto loader = steplens::io::makeLoader(file);
    ASSERT_TRUE(static_cast<bool>(loader)) << tag;
    auto loaded = loader->load(file, {}, sink, steplens::CancelToken{});
    phase(std::format("file={} round={} phase=load_done", tag, round));
    ASSERT_TRUE(loaded.ok()) << tag << " " << loaded.error().message;
    auto built =
        steplens::model::DocumentModel::build(std::move(loaded.value()), sink, steplens::CancelToken{});
    phase(std::format("file={} round={} phase=build_done", tag, round));
    ASSERT_TRUE(built.ok()) << tag << " " << built.error().message;
    auto model = std::move(built.value());
    phase(std::format("file={} round={} phase=mesh_begin prototypes={}", tag, round,
                      model->prototypes().size()));

    std::vector<steplens::mesh::MeshBatch> batches;
    {
        auto scheduler = steplens::occt::makeOcctTaskScheduler(4);
        steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
        // 不在测试里给 push_back 加锁。和 ManySmallPrototypes 一样，只靠 run 把 onBatch 串行化。
        const auto meshed = tessellator.run(
            *model, steplens::mesh::MeshLevel::SchemeB,
            [&](steplens::mesh::MeshBatch&& batch) { batches.push_back(std::move(batch)); }, sink,
            steplens::CancelToken{});
        ASSERT_TRUE(meshed.ok()) << tag << " " << meshed.error().message;
        EXPECT_GT(meshed.value().triangleCount, 0u) << tag;
        EXPECT_EQ(batches.size(), model->prototypes().size()) << tag;
    }
    phase(std::format("file={} round={} phase=mesh_done", tag, round));
    batches.clear();

    phase(std::format("file={} round={} phase=retire_begin", tag, round));
    steplens::model::retireDocument(std::move(model));
    ASSERT_TRUE(steplens::model::waitForDocumentRelease(std::chrono::seconds(180))) << tag;
    phase(std::format("file={} round={} phase=retire_done", tag, round));
    steplens::io::releaseFreedHeap();
    phase(std::format("file={} round={} phase=reclaim_done", tag, round));
}

void runBoxGrid(int boxCount, std::string_view tag) {
    if (!reproEnabled()) {
        GTEST_SKIP() << "STEPLENS_STEP_HEAP_REPRO 未设为 1。windows-build 的独立步骤才会真实执行";
    }
    phase(std::format("file={} phase=start boxes={} rounds={}", tag, boxCount, kRounds));
    const auto directory = std::filesystem::temp_directory_path() / "steplens-step-heap-repro";
    std::filesystem::create_directories(directory);
    const auto file = directory / (std::string(tag) + ".step");
    ASSERT_TRUE(writeBoxGridStep(file, boxCount)) << tag;
    phase(std::format("file={} phase=write_done bytes={}", tag, std::filesystem::file_size(file)));
    for (int round = 0; round < kRounds; ++round) {
        loadMeshRetireReclaim(file, tag, round);
    }
}

} // namespace

TEST(StepHeapRepro, Boxes400) {
    runBoxGrid(400, "box-400");
}

TEST(StepHeapRepro, Boxes1200) {
    runBoxGrid(1200, "box-1200");
}
