#include "BenchRun.h"
#include "SampleCatalog.h"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
#include <shellapi.h>
// clang-format on
#endif

namespace {
constexpr std::string_view kUsage =
    "用法: steplens-bench --file X-02 --stages all --repeat 3 --json out.json\n"
    "  --file    样例编号（如 X-02，从 STEPLENS_SAMPLES 找）或文件路径\n"
    "  --stages  all，或 load（读到模型，不网格、不出图），或逗号分隔的\n"
    "            reading,model,mesh,scene。后一阶段会带上它依赖的前面阶段\n"
    "  --repeat  重复次数。报告各次数值的中位数；3 次取排序后的中间一次\n"
    "  --json    写出的 JSON 路径\n"
    "T07 只有方案 B 这一级网格。fine_triangle_count 固定为 0。\n";

struct Options {
    std::string file;
    std::string stages = "all";
    int repeat = 1;
    std::filesystem::path json;
};

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(text.data()), text.size());
    return std::filesystem::path(bytes);
}

std::optional<std::filesystem::path> samplesRoot() {
#if defined(_WIN32)
    constexpr DWORD kCapacity = 32767;
    std::vector<wchar_t> buffer(kCapacity, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"STEPLENS_SAMPLES", buffer.data(), kCapacity);
    if (length == 0 || length >= kCapacity) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer.data());
#else
    const char* root = std::getenv("STEPLENS_SAMPLES");
    if (root == nullptr || root[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(root);
#endif
}

bool parseRepeat(std::string_view text, int& repeat, std::string& error) {
    int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < 1) {
        error = "--repeat 必须是正整数";
        return false;
    }
    repeat = value;
    return true;
}

bool parseStages(std::string_view text, steplens::bench::StageMask& stages, std::string& error) {
    stages = {};
    if (text == "all") {
        stages.load = true;
        stages.model = true;
        stages.mesh = true;
        stages.present = true;
        return true;
    }
    if (text == "load") {
        stages.load = true;
        stages.model = true;
        return true;
    }
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t comma = text.find(',', cursor);
        const std::string_view token =
            text.substr(cursor, comma == std::string_view::npos ? text.size() - cursor : comma - cursor);
        if (token == "reading" || token == "parsing" || token == "transferring" || token == "transfer") {
            stages.load = true;
        } else if (token == "model" || token == "building_model") {
            stages.model = true;
        } else if (token == "mesh" || token == "meshing") {
            stages.mesh = true;
        } else if (token == "scene" || token == "presenting") {
            stages.present = true;
        } else if (!token.empty()) {
            error = "不认识的阶段: " + std::string(token);
            return false;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        cursor = comma + 1;
    }
    if (!stages.load && !stages.model && !stages.mesh && !stages.present) {
        error = "--stages 是空的";
        return false;
    }
    if (stages.present) {
        stages.mesh = true;
    }
    if (stages.mesh) {
        stages.model = true;
    }
    if (stages.model) {
        stages.load = true;
    }
    return true;
}

bool parseArgs(const std::vector<std::string>& args, Options& options, std::string& error) {
    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto need = [&](std::string_view flag) -> const std::string* {
            if (index + 1 >= args.size()) {
                error = std::string(flag) + " 缺少参数";
                return nullptr;
            }
            ++index;
            return &args[index];
        };
        if (arg == "--help" || arg == "-h") {
            error.clear();
            return false;
        }
        if (arg == "--file") {
            const std::string* value = need(arg);
            if (value == nullptr) {
                return false;
            }
            options.file = *value;
        } else if (arg == "--stages") {
            const std::string* value = need(arg);
            if (value == nullptr) {
                return false;
            }
            options.stages = *value;
        } else if (arg == "--repeat") {
            const std::string* value = need(arg);
            if (value == nullptr) {
                return false;
            }
            if (!parseRepeat(*value, options.repeat, error)) {
                return false;
            }
        } else if (arg == "--json") {
            const std::string* value = need(arg);
            if (value == nullptr) {
                return false;
            }
            options.json = pathFromUtf8(*value);
        } else {
            error = "不认识的参数: " + arg;
            return false;
        }
    }
    if (options.file.empty() || options.json.empty()) {
        error = "需要 --file 和 --json";
        return false;
    }
    return true;
}

std::filesystem::path resolveFile(const std::string& argument, std::string& sampleId, std::string& error) {
    std::error_code status;
    const std::filesystem::path direct = pathFromUtf8(argument);
    if (std::filesystem::is_regular_file(direct, status)) {
        return direct;
    }
    if (const std::optional<std::string_view> filename = steplens::bench::filenameForSample(argument)) {
        sampleId = argument;
        const std::optional<std::filesystem::path> root = samplesRoot();
        if (!root) {
            error = "STEPLENS_SAMPLES 未设置，无法解析样例编号 " + argument;
            return {};
        }
        const std::filesystem::path file = *root / pathFromUtf8(*filename);
        if (!std::filesystem::is_regular_file(file, status)) {
            error = "样例目录里没有 " + std::string(*filename);
            return {};
        }
        return file;
    }
    error = "找不到文件 " + argument;
    return {};
}

int run(const std::vector<std::string>& args) {
    Options options;
    std::string error;
    if (!parseArgs(args, options, error)) {
        if (!error.empty()) {
            std::cerr << error << "\n";
        }
        std::cerr << kUsage;
        return error.empty() ? 0 : 2;
    }
    steplens::bench::StageMask stages;
    if (!parseStages(options.stages, stages, error)) {
        std::cerr << error << "\n" << kUsage;
        return 2;
    }
    std::string sampleId;
    const std::filesystem::path file = resolveFile(options.file, sampleId, error);
    if (file.empty()) {
        std::cerr << error << "\n";
        return 1;
    }

    steplens::bench::BenchOutput output = steplens::bench::runBench(file, stages, options.repeat);
    output.stagesRequested = options.stages;
    const std::string json = steplens::bench::benchJson(output, options.file, sampleId);
    std::ofstream stream(options.json, std::ios::binary);
    if (!stream) {
        std::cerr << "无法写入 " << pathUtf8(options.json) << "\n";
        return 1;
    }
    stream << json;
    stream.close();
    if (!stream) {
        std::cerr << "写入 JSON 失败 " << pathUtf8(options.json) << "\n";
        return 1;
    }
    if (!output.ok) {
        std::cerr << output.error << "\n";
        std::cout << "ok=0 json=" << pathUtf8(options.json) << "\n";
        return 1;
    }
    const auto& runs = output.runs;
    const auto reading = steplens::bench::medianOf<double>([&] {
        std::vector<double> values;
        values.reserve(runs.size());
        for (const auto& run : runs) {
            values.push_back(run.stages.reading);
        }
        return values;
    }());
    const auto prototype = steplens::bench::medianOf<std::int64_t>([&] {
        std::vector<std::int64_t> values;
        values.reserve(runs.size());
        for (const auto& run : runs) {
            values.push_back(run.counts.prototypeCount);
        }
        return values;
    }());
    const auto instances = steplens::bench::medianOf<std::int64_t>([&] {
        std::vector<std::int64_t> values;
        for (const auto& run : runs) {
            values.push_back(run.counts.instanceCount);
        }
        return values;
    }());
    const auto faces = steplens::bench::medianOf<std::int64_t>([&] {
        std::vector<std::int64_t> values;
        for (const auto& run : runs) {
            values.push_back(run.counts.faceCount);
        }
        return values;
    }());
    const auto coarse = steplens::bench::medianOf<std::int64_t>([&] {
        std::vector<std::int64_t> values;
        for (const auto& run : runs) {
            values.push_back(run.counts.coarseTriangleCount);
        }
        return values;
    }());
    const auto fine = steplens::bench::medianOf<std::int64_t>([&] {
        std::vector<std::int64_t> values;
        for (const auto& run : runs) {
            values.push_back(run.counts.fineTriangleCount);
        }
        return values;
    }());
    const auto colors = steplens::bench::medianOf<std::int64_t>([&] {
        std::vector<std::int64_t> values;
        for (const auto& run : runs) {
            values.push_back(run.counts.colorGroupCount);
        }
        return values;
    }());
    std::cout << "ok=1 file=" << pathUtf8(file) << " bytes=" << output.fileBytes
              << " repeat=" << output.repeat << " aggregation=median reading_seconds=" << reading
              << " prototype_count=" << prototype << " instance_count=" << instances
              << " face_count=" << faces << " coarse_triangle_count=" << coarse
              << " fine_triangle_count=" << fine << " color_group_count=" << colors
              << " json=" << pathUtf8(options.json) << "\n";
    return 0;
}

} // namespace

#if defined(_WIN32)
int main() {
    int argc = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wide == nullptr) {
        std::cerr << "无法读取命令行\n";
        return 2;
    }
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        args.push_back(pathUtf8(std::filesystem::path(wide[index])));
    }
    LocalFree(wide);
    return run(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        args.emplace_back(argv[index] == nullptr ? "" : argv[index]);
    }
    return run(args);
}
#endif
