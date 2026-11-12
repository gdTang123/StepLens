#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// 只在 windows-build 的独立步骤里设 STEPLENS_STEP_HEAP_REPRO=1。
// 普通 unit 预设不设这个变量，测试跳过，避免把「Configure, build, test」和内存夹具闸打红。
// 产品代码不在这里改。回收堆走当前分支上产品在用的那个函数：
// #23 是 steplens::io::releaseFreedHeap；main 45d96a7 没有公开头，用 io::detail::releaseFreedHeap
// （ReaderSlot 析构里的同一个函数）。后台 Close 都是 retireDocument + waitForDocumentRelease。

#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "mesh/Tessellator.h"
#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"

#if __has_include("io/HeapRelease.h")
#include "io/HeapRelease.h"
#define STEPLENS_REPRO_PUBLIC_HEAP 1
#else
namespace steplens::io::detail {
void releaseFreedHeap() noexcept;
using HeapPendingProbe = int (*)() noexcept;
void setHeapPendingProbe(HeapPendingProbe probe) noexcept;
} // namespace steplens::io::detail
#endif

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    unsigned long code = 0;
    void* address = nullptr;
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        code = info->ExceptionRecord->ExceptionCode;
        address = info->ExceptionRecord->ExceptionAddress;
    }
    std::fprintf(stderr, "crash_exception code=0x%08lx address=%p\n", code, address);
    std::fflush(stderr);
    if (info != nullptr && info->ContextRecord != nullptr) {
        char buffer[8192];
        buffer[0] = '\0';
        Standard::StackTrace(buffer, static_cast<int>(sizeof(buffer)), 40, info->ContextRecord, 0);
        std::fprintf(stderr, "crash_stack_begin\n%s\ncrash_stack_end\n", buffer);
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void installCrashFilter() {
    SetUnhandledExceptionFilter(crashFilter);
}
#else
void installCrashFilter() {}
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

void reclaimHeap() noexcept {
#if defined(STEPLENS_REPRO_PUBLIC_HEAP)
    steplens::io::releaseFreedHeap();
#else
    steplens::io::detail::releaseFreedHeap();
#endif
}

const char* reclaimApi() {
#if defined(STEPLENS_REPRO_PUBLIC_HEAP)
    return "io::releaseFreedHeap";
#else
    return "io::detail::releaseFreedHeap";
#endif
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

    phase(std::format("file={} round={} phase=mesh_begin", tag, round));
    std::vector<steplens::mesh::MeshBatch> batches;
    {
        auto scheduler = steplens::occt::makeOcctTaskScheduler(2);
        steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
        const auto meshed = tessellator.run(
            *model, steplens::mesh::MeshLevel::SchemeB,
            [&](steplens::mesh::MeshBatch&& batch) { batches.push_back(std::move(batch)); }, sink,
            steplens::CancelToken{});
        ASSERT_TRUE(meshed.ok()) << tag << " " << meshed.error().message;
        EXPECT_GT(meshed.value().triangleCount, 0u) << tag;
    }
    phase(std::format("file={} round={} phase=mesh_done", tag, round));
    batches.clear();

    phase(std::format("file={} round={} phase=retire_begin", tag, round));
    steplens::model::retireDocument(std::move(model));
    ASSERT_TRUE(steplens::model::waitForDocumentRelease(std::chrono::seconds(180))) << tag;
    phase(std::format("file={} round={} phase=retire_done", tag, round));
    reclaimHeap();
    phase(std::format("file={} round={} phase=reclaim_done", tag, round));
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    SetEnvironmentVariableA(name, value);
#else
    if (setenv(name, value, 1) != 0) {
        ADD_FAILURE() << name;
    }
#endif
}

std::string envOrUnset(const char* name) {
#if defined(_WIN32)
    char buffer[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length == 0 || length >= sizeof(buffer)) {
        return "unset";
    }
    return std::string(buffer, length);
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return "unset";
    }
    return std::string(value);
#endif
}

void prepareVariant(const char* variant) {
    installCrashFilter();
    steplens::io::detail::setHeapPendingProbe(&steplens::model::documentsPendingRelease);
    setEnv("STEPLENS_REPRO_NO_TRIM", "0");
    setEnv("STEPLENS_REPRO_SKIP_TRIM_IF_PENDING", "0");
    setEnv("STEPLENS_REPRO_CLEAR_BEFORE_CLOSE", "0");
    setEnv("STEPLENS_REPRO_PURGE_BEFORE_TRIM", "0");
    setEnv("STEPLENS_REPRO_NO_PARALLEL", "0");
    if (std::strcmp(variant, "no-trim") == 0) {
        setEnv("STEPLENS_REPRO_NO_TRIM", "1");
    } else if (std::strcmp(variant, "clear-before-close") == 0) {
        setEnv("STEPLENS_REPRO_CLEAR_BEFORE_CLOSE", "1");
    } else if (std::strcmp(variant, "skip-if-pending") == 0) {
        setEnv("STEPLENS_REPRO_SKIP_TRIM_IF_PENDING", "1");
    } else if (std::strcmp(variant, "purge-before-trim") == 0) {
        setEnv("STEPLENS_REPRO_PURGE_BEFORE_TRIM", "1");
    } else if (std::strcmp(variant, "no-parallel") == 0) {
        setEnv("STEPLENS_REPRO_NO_PARALLEL", "1");
    }
    phase(std::format("variant={} mmgt_opt={} mmgt_clear={} mmgt_mmap={} note=no Interface_Static or "
                      "XSControl reset between rounds",
                      variant, envOrUnset("MMGT_OPT"), envOrUnset("MMGT_CLEAR"), envOrUnset("MMGT_MMAP")));
}

void runBoxGrid(int boxCount, std::string_view tag) {
    if (!reproEnabled()) {
        GTEST_SKIP() << "STEPLENS_STEP_HEAP_REPRO 未设为 1。windows-build 的独立步骤才会真实执行";
    }
    phase(std::format("file={} phase=start boxes={} rounds={} reclaim_api={}", tag, boxCount, kRounds,
                      reclaimApi()));
    const auto directory = std::filesystem::temp_directory_path() / "steplens-step-heap-repro";
    std::filesystem::create_directories(directory);
    const auto file = directory / (std::string(tag) + ".step");
    phase(std::format("file={} phase=write_begin boxes={}", tag, boxCount));
    ASSERT_TRUE(writeBoxGridStep(file, boxCount)) << tag;
    phase(std::format("file={} phase=write_done bytes={}", tag, std::filesystem::file_size(file)));
    for (int round = 0; round < kRounds; ++round) {
        loadMeshRetireReclaim(file, tag, round);
    }
}

} // namespace

void runVariant(const char* variant) {
    if (!reproEnabled()) {
        GTEST_SKIP() << "STEPLENS_STEP_HEAP_REPRO 未设为 1。windows-build 的独立步骤才会真实执行";
    }
    prepareVariant(variant);
    runBoxGrid(400, variant);
}

TEST(StepHeapRepro, Baseline400x10) {
    runVariant("baseline");
}

TEST(StepHeapRepro, NoTrim400x10) {
    runVariant("no-trim");
}

TEST(StepHeapRepro, ClearBeforeClose400x10) {
    runVariant("clear-before-close");
}

TEST(StepHeapRepro, SkipTrimIfPending400x10) {
    runVariant("skip-if-pending");
}

TEST(StepHeapRepro, PurgeBeforeTrim400x10) {
    runVariant("purge-before-trim");
}

TEST(StepHeapRepro, NoParallel400x10) {
    runVariant("no-parallel");
}
