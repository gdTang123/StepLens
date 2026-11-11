#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "io/HeapRelease.h"
#include "io/ILoader.h"
#include "mesh/Tessellator.h"
#include "model/DocumentModel.h"
#include "occtsupport/OcctBoundary.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
#include <psapi.h>
// clang-format on
#pragma comment(lib, "psapi.lib")
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {

constexpr std::int64_t kBytesPerKb = 1024;
constexpr std::int64_t kCloseToleranceBytes = 300LL * 1024LL * 1024LL;
constexpr int kRounds = 3;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitizer = true;
#else
constexpr bool kSanitizer = false;
#endif

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

#if defined(_WIN32)
std::int64_t privateBytesNow() {
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info),
                             sizeof(info)) == 0) {
        return -1;
    }
    return static_cast<std::int64_t>(info.PrivateUsage);
}
#else
std::int64_t labeledKilobytes(const std::string& line) {
    const char* begin = line.c_str();
    const char* end = begin + line.size();
    while (begin < end && (*begin < '0' || *begin > '9')) {
        ++begin;
    }
    if (begin == end) {
        return -1;
    }
    char* parsedEnd = nullptr;
    const long long parsed = std::strtoll(begin, &parsedEnd, 10);
    if (parsedEnd == begin) {
        return -1;
    }
    return static_cast<std::int64_t>(parsed);
}

std::int64_t privateBytesNow() {
    std::ifstream file("/proc/self/smaps_rollup");
    if (!file) {
        return -1;
    }
    std::int64_t clean = -1;
    std::int64_t dirty = -1;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("Private_Clean:", 0) == 0) {
            clean = labeledKilobytes(line);
        } else if (line.rfind("Private_Dirty:", 0) == 0) {
            dirty = labeledKilobytes(line);
        }
    }
    if (clean < 0 || dirty < 0) {
        return -1;
    }
    return (clean + dirty) * kBytesPerKb;
}
#endif

std::optional<std::filesystem::path> samplesDirectory() {
#if defined(_WIN32)
    constexpr DWORD kCapacity = 32767;
    wchar_t buffer[kCapacity];
    const DWORD length = GetEnvironmentVariableW(L"STEPLENS_SAMPLES", buffer, kCapacity);
    if (length == 0 || length >= kCapacity) {
        return std::nullopt;
    }
    return std::filesystem::path{buffer};
#else
    const char* root = std::getenv("STEPLENS_SAMPLES");
    if (root == nullptr || root[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path{root};
#endif
}

std::optional<std::filesystem::path> findNamedSample(const std::filesystem::path& root,
                                                     const std::string& name) {
    const std::filesystem::path direct = root / name;
    if (std::filesystem::exists(direct)) {
        return direct;
    }
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end; it != end;
         it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        constexpr int kMaxDepth = 3;
        if (it.depth() > kMaxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(error) && it->path().filename().string() == name) {
            return it->path();
        }
    }
    return std::nullopt;
}

bool writeBoxStep(const std::filesystem::path& path) {
    STEPControl_Writer writer;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    if (writer.Transfer(box, STEPControl_AsIs) != IFSelect_RetDone) {
        return false;
    }
    return writer.Write(pathUtf8(path).c_str()) == IFSelect_RetDone;
}

void openCloseOnce(const std::filesystem::path& file) {
    const auto bytes = std::filesystem::file_size(file);
    steplens::ProgressSink sink;
    sink.reset(static_cast<std::uint64_t>(bytes));
    auto loader = steplens::io::makeLoader(file);
    ASSERT_TRUE(static_cast<bool>(loader));
    auto loaded = loader->load(file, {}, sink, steplens::CancelToken{});
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    auto built =
        steplens::model::DocumentModel::build(std::move(loaded.value()), sink, steplens::CancelToken{});
    ASSERT_TRUE(built.ok()) << built.error().message;
    auto model = std::move(built.value());
    std::vector<steplens::mesh::MeshBatch> batches;
    {
        auto scheduler = steplens::occt::makeOcctTaskScheduler(2);
        steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
        const auto meshed = tessellator.run(
            *model, steplens::mesh::MeshLevel::SchemeB,
            [&](steplens::mesh::MeshBatch&& batch) { batches.push_back(std::move(batch)); }, sink,
            steplens::CancelToken{});
        ASSERT_TRUE(meshed.ok()) << pathUtf8(file) << " " << meshed.error().message;
        EXPECT_GT(meshed.value().triangleCount, 0u) << pathUtf8(file);
    }
    int shownVertices = 0;
    for (const auto& batch : batches) {
        for (const auto& group : batch.groups) {
            if (!group.triangles.IsNull()) {
                shownVertices += group.triangles->VertexNumber();
            }
        }
    }
    EXPECT_GT(shownVertices, 0) << pathUtf8(file);
    batches.clear();
    model.reset();
    steplens::io::releaseFreedHeap();
}

void expectNearBaseline(std::int64_t baseline, std::string_view label) {
    const std::int64_t now = privateBytesNow();
    ASSERT_GE(now, 0) << label;
    EXPECT_LE(now, baseline + kCloseToleranceBytes) << label << " now=" << now << " baseline=" << baseline;
}

} // namespace

TEST(MemoryRelease, SyntheticOpenCloseStaysNearBaseline) {
    const auto directory = std::filesystem::temp_directory_path() / "steplens-memory-release-synthetic";
    std::filesystem::create_directories(directory);
    const auto file = directory / "box.step";
    ASSERT_TRUE(writeBoxStep(file));

    steplens::io::releaseFreedHeap();
    const std::int64_t baseline = privateBytesNow();
    ASSERT_GE(baseline, 0);

    for (int round = 0; round < kRounds; ++round) {
        openCloseOnce(file);
        expectNearBaseline(baseline, "synthetic round " + std::to_string(round));
    }

    std::error_code removed;
    std::filesystem::remove_all(directory, removed);
}

TEST(MemoryRelease, SampleOpenCloseStaysNearBaseline) {
    const auto root = samplesDirectory();
    if (!root) {
        GTEST_SKIP() << "STEPLENS_SAMPLES 未设置";
    }

    constexpr std::string_view kNames[] = {
        "screw.step",
        "linkrods.step",
        "as1-ac-214.stp",
        "Schenkel.stp",
        "as1_pe.stp",
        "moon_buggy_asm.stp",
        "123Block_Color.stp",
        "nist_ctc_02_asme1_ap242-e2.stp",
        "nist_ftc_09_asme1_ap242-e1.stp",
        "nist_stc_10_asme1_ap242-e2.stp",
        "ROTOR-201NAL-Z7.STEP",
        "bearing.iges",
        "hammer.iges",
        "1030490B.igs",
        "Rocky_House.stp",
        "SO101 Assembly.step",
        "YARR_assembly.STEP",
        "Cruise_Assembly.stp",
        "SP218P001.IGS",
        "1026868C.IGS",
        "PX13298F.IGS",
    };

    std::vector<std::filesystem::path> files;
    for (const std::string_view name : kNames) {
        if (const auto found = findNamedSample(*root, std::string(name))) {
            files.push_back(*found);
        }
    }
    if (files.empty()) {
        GTEST_SKIP() << "STEPLENS_SAMPLES 里没有 S/M 档样例";
    }

    steplens::io::releaseFreedHeap();
    const std::int64_t baseline = privateBytesNow();
    ASSERT_GE(baseline, 0);
    for (const auto& file : files) {
        for (int round = 0; round < kRounds; ++round) {
            openCloseOnce(file);
            expectNearBaseline(baseline, pathUtf8(file) + " round " + std::to_string(round));
        }
    }
}

#if defined(__GLIBC__)
TEST(HeapRelease, MallocTrimDoesNotGrowAfterFree) {
    if (kSanitizer) {
        GTEST_SKIP() << "ASan/TSan 替换了分配器，malloc_trim 不能代表产品路径";
    }
    // 这块 glibc 在 free 时已经把大块还回去了，trim 只扫剩余空闲页。
    // 这里锁的是调用本身不把私有字节抬高；下降幅度由 T16 的文件测量给出。
    ASSERT_EQ(mallopt(M_MMAP_THRESHOLD, 1 << 30), 1);
    void* pin = std::malloc(1U << 20);
    ASSERT_NE(pin, nullptr);
    std::memset(pin, 1, 1U << 20);
    constexpr int kChunks = 16;
    constexpr std::size_t kChunk = 4U * 1024U * 1024U;
    std::vector<void*> blocks(static_cast<std::size_t>(kChunks), nullptr);
    for (int index = 0; index < kChunks; ++index) {
        void* block = std::malloc(kChunk);
        ASSERT_NE(block, nullptr);
        std::memset(block, 1, kChunk);
        blocks[static_cast<std::size_t>(index)] = block;
    }
    for (void* block : blocks) {
        std::free(block);
    }
    const std::int64_t held = privateBytesNow();
    steplens::io::releaseFreedHeap();
    const std::int64_t after = privateBytesNow();
    std::free(pin);
    ASSERT_GE(held, 0);
    ASSERT_GE(after, 0);
    EXPECT_LE(after, held + kBytesPerKb * kBytesPerKb);
}
#endif
