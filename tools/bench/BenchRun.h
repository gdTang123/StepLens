#pragma once

#include "MemoryProbe.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace steplens::bench {

struct StageSeconds {
    double reading = 0;
    double parsing = 0;
    double transferring = 0;
    double buildingModel = 0;
    double meshing = 0;
    double presenting = 0;
};

struct RunCounts {
    std::int64_t prototypeCount = 0;
    std::int64_t instanceCount = 0;
    std::int64_t faceCount = 0;
    std::int64_t nodeCount = 0;
    std::int64_t coarseTriangleCount = 0;
    std::int64_t fineTriangleCount = 0;
    std::int64_t colorGroupCount = 0;
    std::int64_t instanceTriangleCount = 0;
};

struct StageMask {
    bool load = false;
    bool model = false;
    bool mesh = false;
    bool present = false;
};

struct RunRecord {
    StageSeconds stages;
    RunCounts counts;
    std::vector<MemoryMetric> metrics;
};

struct BenchOutput {
    bool ok = false;
    std::string error;
    std::string occtVersion;
    std::string schema;
    std::string stagesRequested;
    std::int64_t fileBytes = 0;
    StageMask stages;
    int repeat = 0;
    std::vector<RunRecord> runs;
    std::vector<MemoryMetric> lifetime;
    std::string platform;
    bool residentReset = false;
};

// 奇数次取排序后正中间的一次。repeat 3 就是三次里的中间值，不平均。
template <class T> T medianOf(std::vector<T> values) {
    if (values.empty()) {
        return T{};
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

[[nodiscard]] BenchOutput runBench(const std::filesystem::path& file, StageMask stages, int repeat);

[[nodiscard]] std::string benchJson(const BenchOutput& output, std::string_view fileArgument,
                                    std::string_view sampleId);

} // namespace steplens::bench
