#include "BenchRun.h"

#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "mesh/MeshPolicy.h"
#include "mesh/Tessellator.h"
#include "model/DocumentModel.h"
#include "occtsupport/OcctBoundary.h"
#include "occtsupport/occt_compat.h"
#include "scene/PrototypePrs.h"
#include "scene/SceneBuilder.h"

#include <AIS_ConnectedInteractive.hxx>
#include <AIS_InteractiveContext.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_Viewer.hxx>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <malloc.h>
#endif

namespace steplens::bench {
namespace {

constexpr double kNsPerSecond = 1000000000.0;
constexpr std::string_view kMeshNote =
    "T07 只有方案 B 单级网格（c = 0.002）。coarse_triangle_count 是这一级的原型三角形数，不乘实例。"
    "fine_triangle_count 为 0：粗/细两级是 T07+，这一版没有第二级网格。";
constexpr std::string_view kMeshSkippedNote =
    "这次没有跑 meshing。coarse_triangle_count 和 fine_triangle_count 都是 0，不是测量值。";

static_assert(static_cast<int>(ProgressPhase::Ready) == 7);

struct PhaseClock {
    std::array<std::int64_t, 8> ns{};
    std::array<std::uint8_t, 8> seen{};
};

void notePhase(ProgressPhase phase, void* user) noexcept {
    auto* clock = static_cast<PhaseClock*>(user);
    if (clock == nullptr) {
        return;
    }
    const auto index = static_cast<std::size_t>(phase);
    if (index >= clock->seen.size() || clock->seen[index] != 0) {
        return;
    }
    clock->seen[index] = 1;
    clock->ns[index] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
}

std::int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::optional<std::int64_t> markOf(const PhaseClock& clock, ProgressPhase phase) {
    const auto index = static_cast<std::size_t>(phase);
    if (index >= clock.seen.size() || clock.seen[index] == 0) {
        return std::nullopt;
    }
    return clock.ns[index];
}

double secondsBetween(std::int64_t start, std::int64_t end) {
    if (end < start) {
        return 0.0;
    }
    return static_cast<double>(end - start) / kNsPerSecond;
}

std::int64_t clampInt64(std::uint64_t value) {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (value > kMax) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void releaseHeap() noexcept {
#if defined(_WIN32)
    _heapmin();
#else
    malloc_trim(0);
#endif
}

void writeLine(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

StageSeconds durationsOf(const PhaseClock& clock, std::int64_t loadEnd, double buildingModel, double meshing,
                         double presenting) {
    StageSeconds stages;
    const auto reading = markOf(clock, ProgressPhase::ReadingBytes);
    const auto parsing = markOf(clock, ProgressPhase::Parsing);
    const auto transfer = markOf(clock, ProgressPhase::Transferring);
    if (reading) {
        const std::int64_t end = parsing ? *parsing : transfer ? *transfer : loadEnd;
        stages.reading = secondsBetween(*reading, end);
    }
    if (parsing) {
        const std::int64_t end = transfer ? *transfer : loadEnd;
        stages.parsing = secondsBetween(*parsing, end);
    }
    if (transfer) {
        stages.transferring = secondsBetween(*transfer, loadEnd);
    }
    stages.buildingModel = buildingModel;
    stages.meshing = meshing;
    stages.presenting = presenting;
    return stages;
}

opencascade::handle<AIS_InteractiveContext> makeContext(std::string& error) {
    try {
        opencascade::handle<Aspect_DisplayConnection> display = new Aspect_DisplayConnection();
        opencascade::handle<OpenGl_GraphicDriver> driver = new OpenGl_GraphicDriver(display, false);
        opencascade::handle<V3d_Viewer> viewer = new V3d_Viewer(driver);
        viewer->SetDefaultLights();
        viewer->SetLightOn();
        opencascade::handle<AIS_InteractiveContext> context = new AIS_InteractiveContext(viewer);
        mesh::MeshBatch empty;
        opencascade::handle<scene::PrototypePrs> probe = new scene::PrototypePrs(empty, TopoDS_Shape{});
        opencascade::handle<AIS_ConnectedInteractive> instance = new AIS_ConnectedInteractive();
        instance->Connect(probe);
        context->Display(instance, scene::PrototypePrs::ShadedWithEdges, 0, false);
        context->RemoveAll(false);
        return context;
    } catch (const Standard_Failure& failure) {
        error = std::string("无法创建 OpenGL 视图: ") + occt::failureMessage(failure);
    } catch (const std::exception& ex) {
        error = std::string("无法创建 OpenGL 视图: ") + ex.what();
    } catch (...) {
        error = "无法创建 OpenGL 视图";
    }
    return {};
}

std::string pumpScene(scene::SceneBuilder& builder, const model::DocumentModel& model) {
    const std::int64_t guard = clampInt64(model.nodeCount()) * 4 + 8;
    std::int64_t pumps = 0;
    while (builder.pending() > 0) {
        const int before = builder.pending();
        builder.pump();
        ++pumps;
        if (builder.failed()) {
            return builder.lastError().empty() ? "场景创建失败" : builder.lastError();
        }
        if (pumps > guard && builder.pending() >= before) {
            return "场景创建没有进展";
        }
    }
    return {};
}

struct OnceResult {
    bool ok = false;
    std::string error;
    std::string schema;
    RunRecord record;
};

OnceResult failOnce(std::string error) {
    OnceResult result;
    result.error = std::move(error);
    return result;
}

OnceResult runOnce(const std::filesystem::path& file, std::int64_t fileBytes, StageMask stages,
                   MemoryProbe& probe) {
    ProgressSink sink;
    sink.reset(static_cast<std::uint64_t>(fileBytes));
    PhaseClock clock;
    sink.setPhaseHook(&notePhase, &clock);

    auto loader = io::makeLoader(file);
    if (!loader) {
        return failOnce("无法选择加载器");
    }
    auto loaded = loader->load(file, {}, sink, CancelToken{});
    const std::int64_t loadEnd = steadyNs();
    sink.setPhaseHook(nullptr, nullptr);
    if (!loaded.ok()) {
        return failOnce(loaded.error().userMessage + " (" + loaded.error().message + ")");
    }
    writeLine(std::format("reader_ok=1 bytes={} schema={}\n", fileBytes, loaded.value().schema));

    OnceResult result;
    result.schema = loaded.value().schema;
    double buildingSeconds = 0;
    double meshingSeconds = 0;
    double presentingSeconds = 0;
    std::shared_ptr<const model::DocumentModel> model;
    std::vector<mesh::MeshBatch> batches;

    if (stages.model) {
        const auto started = std::chrono::steady_clock::now();
        auto built = model::DocumentModel::build(std::move(loaded.value()), sink, CancelToken{});
        buildingSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!built.ok()) {
            OnceResult failed = failOnce(built.error().userMessage + " (" + built.error().message + ")");
            failed.schema = result.schema;
            return failed;
        }
        model = std::move(built.value());
        result.record.counts.prototypeCount = clampInt64(model->prototypes().size());
        result.record.counts.nodeCount = clampInt64(model->nodeCount());
        for (const model::PrototypeInfo& info : model->prototypes()) {
            result.record.counts.instanceCount += static_cast<std::int64_t>(info.instanceCount);
            result.record.counts.faceCount += static_cast<std::int64_t>(info.faceCount);
            result.record.counts.colorGroupCount += clampInt64(info.colorGroups.size());
        }
    }

    if (stages.mesh) {
        if (!model) {
            return failOnce("网格阶段缺少模型");
        }
        const auto started = std::chrono::steady_clock::now();
        auto scheduler = occt::makeOcctTaskScheduler();
        mesh::Tessellator tessellator(*scheduler, mesh::MeshPolicy{});
        std::mutex batchesMutex;
        auto meshed = tessellator.run(
            *model, mesh::MeshLevel::SchemeB,
            [&](mesh::MeshBatch&& batch) {
                std::lock_guard lock(batchesMutex);
                batches.push_back(std::move(batch));
            },
            sink, CancelToken{});
        meshingSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!meshed.ok()) {
            OnceResult failed = failOnce(meshed.error().userMessage + " (" + meshed.error().message + ")");
            failed.schema = result.schema;
            return failed;
        }
        result.record.counts.coarseTriangleCount = clampInt64(meshed.value().triangleCount);
        result.record.counts.instanceTriangleCount = clampInt64(meshed.value().instanceTriangleCount);
        result.record.counts.fineTriangleCount = 0;
    }

    if (stages.present) {
        if (!model) {
            return failOnce("出图阶段缺少模型");
        }
        const auto started = std::chrono::steady_clock::now();
        std::string viewError;
        auto context = makeContext(viewError);
        if (context.IsNull()) {
            OnceResult failed = failOnce(viewError.empty() ? "无法创建 OpenGL 视图" : viewError);
            failed.schema = result.schema;
            return failed;
        }
        scene::SceneBuilder builder(context);
        builder.reset(model);
        for (mesh::MeshBatch& batch : batches) {
            builder.enqueue(std::move(batch));
        }
        const std::string pumpError = pumpScene(builder, *model);
        presentingSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!pumpError.empty()) {
            OnceResult failed = failOnce(pumpError);
            failed.schema = result.schema;
            return failed;
        }
    }

    result.record.stages = durationsOf(clock, loadEnd, buildingSeconds, meshingSeconds, presentingSeconds);
    result.record.metrics = probe.runMetrics();
    result.ok = true;
    releaseHeap();
    return result;
}

void appendEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (unsigned char ch : text) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(ch));
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

void appendMetric(std::string& out, const MemoryMetric& metric) {
    out += "{\"name\":";
    appendEscaped(out, metric.name);
    out += ",\"role\":";
    appendEscaped(out, metric.role);
    out += ",\"source\":";
    appendEscaped(out, metric.source);
    out += ",\"corresponds_to\":";
    appendEscaped(out, metric.correspondsTo);
    out += ",\"aggregation\":";
    appendEscaped(out, metric.aggregation);
    out += std::format(",\"available\":{},\"bytes\":{}}}", metric.available ? "true" : "false", metric.bytes);
}

std::vector<MemoryMetric> medianMetrics(const std::vector<RunRecord>& runs) {
    std::vector<MemoryMetric> median;
    if (runs.empty()) {
        return median;
    }
    for (const MemoryMetric& metric : runs.front().metrics) {
        std::vector<std::int64_t> values;
        values.reserve(runs.size());
        for (const RunRecord& run : runs) {
            for (const MemoryMetric& candidate : run.metrics) {
                if (candidate.name == metric.name) {
                    values.push_back(candidate.bytes);
                }
            }
        }
        MemoryMetric combined = metric;
        combined.bytes = medianOf(std::move(values));
        combined.aggregation = "median";
        median.push_back(std::move(combined));
    }
    return median;
}

template <class Read> auto medianField(const std::vector<RunRecord>& runs, Read read) {
    using Value = std::remove_cvref_t<decltype(read(runs.front()))>;
    std::vector<Value> values;
    values.reserve(runs.size());
    for (const RunRecord& run : runs) {
        values.push_back(read(run));
    }
    return medianOf(std::move(values));
}

void appendRun(std::string& out, const RunRecord& run) {
    const StageSeconds& stages = run.stages;
    const RunCounts& counts = run.counts;
    out += std::format(
        "{{\"reading_seconds\":{:.9f},\"parsing_seconds\":{:.9f},\"transferring_seconds\":{:.9f},"
        "\"building_model_seconds\":{:.9f},\"meshing_seconds\":{:.9f},\"presenting_seconds\":{:.9f},"
        "\"prototype_count\":{},\"instance_count\":{},\"face_count\":{},\"node_count\":{},"
        "\"coarse_triangle_count\":{},\"fine_triangle_count\":{},\"color_group_count\":{},"
        "\"instance_triangle_count\":{}}}",
        stages.reading, stages.parsing, stages.transferring, stages.buildingModel, stages.meshing,
        stages.presenting, counts.prototypeCount, counts.instanceCount, counts.faceCount, counts.nodeCount,
        counts.coarseTriangleCount, counts.fineTriangleCount, counts.colorGroupCount,
        counts.instanceTriangleCount);
}

} // namespace

BenchOutput runBench(const std::filesystem::path& file, StageMask stages, int repeat) {
    BenchOutput output;
    output.occtVersion = occt::versionString();
    output.stages = stages;
    output.repeat = repeat;
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(file, sizeError);
    if (sizeError) {
        output.error = "无法读取文件大小: " + pathUtf8(file);
        return output;
    }
    output.fileBytes = clampInt64(static_cast<std::uint64_t>(size));

    MemoryProbe probe;
    output.platform = probe.platform();
    for (int index = 0; index < repeat; ++index) {
        probe.resetRun();
        OnceResult once = runOnce(file, output.fileBytes, stages, probe);
        if (output.schema.empty()) {
            output.schema = once.schema;
        }
        if (!once.ok) {
            output.error = std::move(once.error);
            output.lifetime = probe.lifetimeMetrics();
            output.residentReset = probe.residentResetSupported();
            return output;
        }
        output.runs.push_back(std::move(once.record));
    }
    output.ok = !output.runs.empty();
    if (!output.ok) {
        output.error = "没有完成任何一次运行";
    }
    output.lifetime = probe.lifetimeMetrics();
    output.residentReset = probe.residentResetSupported();
    return output;
}

std::string benchJson(const BenchOutput& output, std::string_view fileArgument, std::string_view sampleId) {
    const bool haveRuns = !output.runs.empty() && output.ok;
    StageSeconds stages;
    RunCounts counts;
    if (haveRuns) {
        stages.reading = medianField(output.runs, [](const RunRecord& run) { return run.stages.reading; });
        stages.parsing = medianField(output.runs, [](const RunRecord& run) { return run.stages.parsing; });
        stages.transferring =
            medianField(output.runs, [](const RunRecord& run) { return run.stages.transferring; });
        stages.buildingModel =
            medianField(output.runs, [](const RunRecord& run) { return run.stages.buildingModel; });
        stages.meshing = medianField(output.runs, [](const RunRecord& run) { return run.stages.meshing; });
        stages.presenting =
            medianField(output.runs, [](const RunRecord& run) { return run.stages.presenting; });
        counts.prototypeCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.prototypeCount; });
        counts.instanceCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.instanceCount; });
        counts.faceCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.faceCount; });
        counts.nodeCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.nodeCount; });
        counts.coarseTriangleCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.coarseTriangleCount; });
        counts.fineTriangleCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.fineTriangleCount; });
        counts.colorGroupCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.colorGroupCount; });
        counts.instanceTriangleCount =
            medianField(output.runs, [](const RunRecord& run) { return run.counts.instanceTriangleCount; });
    }

    std::string out;
    out.reserve(4096);
    out += std::format("{{\"ok\":{},\"tool\":\"steplens-bench\",\"occt_version\":",
                       output.ok ? "true" : "false");
    appendEscaped(out, output.occtVersion);
    out += ",\"file_argument\":";
    appendEscaped(out, fileArgument);
    out += ",\"sample_id\":";
    if (sampleId.empty()) {
        out += "null";
    } else {
        appendEscaped(out, sampleId);
    }
    out += ",\"schema\":";
    appendEscaped(out, output.schema);
    out += std::format(",\"file_bytes\":{},\"stages_requested\":", output.fileBytes);
    appendEscaped(out, output.stagesRequested);
    out += std::format(",\"repeat\":{},\"aggregation\":\"median\"", output.repeat);
    out += std::format(
        ",\"reading_seconds\":{:.9f},\"parsing_seconds\":{:.9f},\"transferring_seconds\":{:.9f},"
        "\"building_model_seconds\":{:.9f},\"meshing_seconds\":{:.9f},\"presenting_seconds\":{:.9f}",
        stages.reading, stages.parsing, stages.transferring, stages.buildingModel, stages.meshing,
        stages.presenting);
    out += std::format(",\"reading_ran\":{},\"model_ran\":{},\"meshing_ran\":{},\"presenting_ran\":{}",
                       output.stages.load ? "true" : "false", output.stages.model ? "true" : "false",
                       output.stages.mesh ? "true" : "false", output.stages.present ? "true" : "false");
    out += std::format(",\"prototype_count\":{},\"instance_count\":{},\"face_count\":{},\"node_count\":{},"
                       "\"coarse_triangle_count\":{},\"fine_triangle_count\":{},\"color_group_count\":{},"
                       "\"instance_triangle_count\":{}",
                       counts.prototypeCount, counts.instanceCount, counts.faceCount, counts.nodeCount,
                       counts.coarseTriangleCount, counts.fineTriangleCount, counts.colorGroupCount,
                       counts.instanceTriangleCount);
    out += ",\"mesh\":{\"levels_implemented\":1,\"scheme\":\"B\",\"deflection_coefficient\":";
    out += std::format("{:.6f}", mesh::kSchemeBLocalCoefficient);
    out += ",\"coarse_triangle_count\":";
    out += std::format("{}", counts.coarseTriangleCount);
    out += ",\"fine_triangle_count\":0,\"coarse_source\":";
    appendEscaped(out, output.stages.mesh ? "scheme_b_prototype_triangles" : "not_run");
    out += ",\"fine_source\":";
    appendEscaped(out, "absent_t07_plus");
    out += ",\"note\":";
    appendEscaped(out, output.stages.mesh ? kMeshNote : kMeshSkippedNote);
    out += "},\"memory\":{\"platform\":";
    appendEscaped(out, output.platform);
    out += ",\"resident_reset\":";
    out += output.residentReset ? "true" : "false";
    out += ",\"metrics\":[";
    bool firstMetric = true;
    if (haveRuns) {
        for (const MemoryMetric& metric : medianMetrics(output.runs)) {
            if (!firstMetric) {
                out.push_back(',');
            }
            firstMetric = false;
            appendMetric(out, metric);
        }
    }
    for (const MemoryMetric& metric : output.lifetime) {
        if (!firstMetric) {
            out.push_back(',');
        }
        firstMetric = false;
        appendMetric(out, metric);
    }
    out += "]},\"runs\":[";
    for (std::size_t index = 0; index < output.runs.size(); ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        appendRun(out, output.runs[index]);
    }
    out += "],\"error\":";
    if (output.error.empty()) {
        out += "null";
    } else {
        appendEscaped(out, output.error);
    }
    out += "}\n";
    return out;
}

} // namespace steplens::bench
