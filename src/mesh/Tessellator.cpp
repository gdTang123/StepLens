#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "mesh/Tessellator.h"

#include "core/Log.h"
#include "occtsupport/OcctBoundary.h"
#include "occtsupport/OcctExceptions.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Poly_Triangulation.hxx>
#include <StdPrs_ShadedShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#if defined(_WIN32)
#include <Windows.h>
#endif
#include <format>
#include <mutex>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace steplens::mesh {
namespace {

struct WorkItem {
    std::vector<std::size_t> indexes;
    std::uint64_t weight = 0;
    bool exclusive = false;
};

// 同一个面或边的 TShape 被两个原型引用时，合成一个任务，避免两边同时写网格。
[[nodiscard]] std::vector<WorkItem> planWork(std::span<const model::PrototypeInfo> prototypes,
                                             std::uint32_t largeFaceCount) {
    const std::size_t count = prototypes.size();
    std::vector<std::size_t> parent(count);
    for (std::size_t index = 0; index < count; ++index) {
        parent[index] = index;
    }
    auto rootOf = [&parent](std::size_t index) {
        std::size_t root = index;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[index] != root) {
            const std::size_t next = parent[index];
            parent[index] = root;
            index = next;
        }
        return root;
    };
    auto unite = [&rootOf, &parent](std::size_t left, std::size_t right) {
        left = rootOf(left);
        right = rootOf(right);
        if (left != right) {
            parent[right] = left;
        }
    };

    std::unordered_map<const void*, std::size_t> owner;
    auto claim = [&](std::size_t prototypeIndex, const TopoDS_Shape& shape, TopAbs_ShapeEnum kind) {
        if (shape.IsNull()) {
            return;
        }
        for (TopExp_Explorer explorer(shape, kind); explorer.More(); explorer.Next()) {
            const TopoDS_Shape& sub = explorer.Current();
            if (sub.IsNull() || sub.TShape().IsNull()) {
                continue;
            }
            const void* key = sub.TShape().get();
            const auto [it, inserted] = owner.emplace(key, prototypeIndex);
            if (!inserted) {
                unite(prototypeIndex, it->second);
            }
        }
    };
    for (std::size_t index = 0; index < count; ++index) {
        claim(index, prototypes[index].shape, TopAbs_FACE);
        claim(index, prototypes[index].shape, TopAbs_EDGE);
    }

    std::unordered_map<std::size_t, WorkItem> grouped;
    for (std::size_t index = 0; index < count; ++index) {
        WorkItem& item = grouped[rootOf(index)];
        item.indexes.push_back(index);
        const auto faces = static_cast<std::uint64_t>(prototypes[index].faceCount);
        const auto instances = static_cast<std::uint64_t>(prototypes[index].instanceCount);
        const std::uint64_t weight = faces * instances;
        if (weight > item.weight) {
            item.weight = weight;
        }
        if (prototypes[index].faceCount > largeFaceCount) {
            item.exclusive = true;
        }
    }

    std::vector<WorkItem> items;
    items.reserve(grouped.size());
    for (auto& entry : grouped) {
        WorkItem& item = entry.second;
        std::sort(item.indexes.begin(), item.indexes.end());
        if (item.indexes.size() > 1) {
            item.exclusive = true;
        }
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const WorkItem& left, const WorkItem& right) {
        if (left.weight != right.weight) {
            return left.weight > right.weight;
        }
        return left.indexes.front() < right.indexes.front();
    });
    return items;
}

[[nodiscard]] float portionOf(double done, std::uint64_t total) {
    if (total == 0) {
        return 1.f;
    }
    if (!std::isfinite(done) || !(done > 0.0)) {
        return 0.f;
    }
    const double ratio = done / static_cast<double>(total);
    if (!(ratio > 0.0)) {
        return 0.f;
    }
    if (ratio > 1.0) {
        return 1.f;
    }
    return static_cast<float>(ratio);
}

class MeshCancelProgress final : public Message_ProgressIndicator {
public:
    MeshCancelProgress(ProgressSink& progress, CancelToken cancel, std::uint64_t doneBefore,
                       std::uint64_t faces, std::uint64_t total)
        : m_progress(progress), m_cancel(std::move(cancel)), m_doneBefore(doneBefore), m_faces(faces),
          m_total(total) {}

    bool UserBreak() override {
        return m_cancel.isCancelled();
    }

protected:
    void Show(const Message_ProgressScope&, const bool) override {
        if (m_cancel.isCancelled()) {
            return;
        }
        float local = static_cast<float>(GetPosition());
        if (!(local > 0.f)) {
            local = 0.f;
        } else if (local > 1.f) {
            local = 1.f;
        }
        const double done =
            static_cast<double>(m_doneBefore) + static_cast<double>(m_faces) * static_cast<double>(local);
        m_progress.setPresentFaceFraction(portionOf(done, m_total));
    }

private:
    ProgressSink& m_progress;
    CancelToken m_cancel;
    std::uint64_t m_doneBefore = 0;
    std::uint64_t m_faces = 0;
    std::uint64_t m_total = 0;
};

class ExclusiveSet {
public:
    void enter(model::PrototypeId id) {
        std::lock_guard lock(m_mutex);
        if (!m_active.insert(id).second) {
            throw std::logic_error("prototype tessellation is not exclusive");
        }
    }

    void leave(model::PrototypeId id) noexcept {
        std::lock_guard lock(m_mutex);
        m_active.erase(id);
    }

private:
    // 保护 m_active。同一个原型的进入和离开都在这把锁下。
    std::mutex m_mutex;
    std::unordered_set<model::PrototypeId> m_active;
};

class ExclusiveLock {
public:
    ExclusiveLock(ExclusiveSet& set, model::PrototypeId id) : m_set(set), m_id(id) {
        m_set.enter(id);
    }

    ~ExclusiveLock() {
        m_set.leave(m_id);
    }

    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    ExclusiveLock(ExclusiveLock&&) = delete;
    ExclusiveLock& operator=(ExclusiveLock&&) = delete;

private:
    ExclusiveSet& m_set;
    model::PrototypeId m_id;
};

struct FaceMesh {
    std::uint64_t triangles = 0;
    std::uint32_t missing = 0;
    std::uint64_t bytes = 0;
};

[[nodiscard]] FaceMesh summarizeMesh(const TopoDS_Shape& shape) {
    FaceMesh result;
    if (shape.IsNull()) {
        return result;
    }
    std::unordered_set<const void*> seen;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        TopLoc_Location location;
        const auto triangulation = BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
        if (triangulation.IsNull() || triangulation->NbTriangles() <= 0) {
            ++result.missing;
            continue;
        }
        result.triangles += static_cast<std::uint64_t>(triangulation->NbTriangles());
        if (!seen.insert(triangulation.get()).second) {
            continue;
        }
        const int nodes = triangulation->NbNodes();
        const int triangles = triangulation->NbTriangles();
        const std::uint64_t coordinateBytes =
            triangulation->IsDoublePrecision() ? sizeof(double) : sizeof(float);
        if (nodes > 0) {
            const auto nodeCount = static_cast<std::uint64_t>(nodes);
            constexpr int kCoordinates = 3;
            constexpr int kUvCoordinates = 2;
            result.bytes += nodeCount * static_cast<std::uint64_t>(kCoordinates) * coordinateBytes;
            if (triangulation->HasNormals()) {
                result.bytes += nodeCount * static_cast<std::uint64_t>(kCoordinates) * sizeof(float);
            }
            if (triangulation->HasUVNodes()) {
                result.bytes += nodeCount * static_cast<std::uint64_t>(kUvCoordinates) * coordinateBytes;
            }
        }
        if (triangles > 0) {
            constexpr int kTriangleCorners = 3;
            result.bytes += static_cast<std::uint64_t>(triangles) *
                            static_cast<std::uint64_t>(kTriangleCorners) * sizeof(int);
        }
    }
    return result;
}

template <class Array> [[nodiscard]] std::uint64_t displayBytes(const opencascade::handle<Array>& array) {
    if (array.IsNull()) {
        return 0;
    }
    std::uint64_t bytes = 0;
    const int vertices = array->VertexNumber();
    if (vertices > 0) {
        constexpr int kPositionComponents = 3;
        constexpr int kNormalComponents = 3;
        const int components =
            array->HasVertexNormals() ? kPositionComponents + kNormalComponents : kPositionComponents;
        bytes +=
            static_cast<std::uint64_t>(vertices) * static_cast<std::uint64_t>(components) * sizeof(float);
    }
    const int edges = array->EdgeNumber();
    if (edges > 0) {
        bytes += static_cast<std::uint64_t>(edges) * sizeof(std::uint32_t);
    }
    return bytes;
}

bool envFlag(const char* name) noexcept {
#if defined(_WIN32)
    char buffer[8] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    return length == 1 && buffer[0] == '1';
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
#endif
}

[[nodiscard]] IMeshTools_Parameters occtParameters(const MeshParams& params) {
    IMeshTools_Parameters parameters;
    parameters.Deflection = params.deflection;
    parameters.Angle = params.angleRad;
    parameters.Relative = false;
    // 对照：400 盒是 2400 面，策略会打开 InParallel。#23 对这种原型改成串行分块。
    parameters.InParallel = params.inParallel && !envFlag("STEPLENS_REPRO_NO_PARALLEL");
    parameters.MinSize = params.deflection * kMinSizeToDeflection;
    // 方案 B 跟摸底一致：不做曲面内部偏差控制，允许用更粗的网格替换旧网格。
    parameters.ControlSurfaceDeflection = false;
    parameters.AllowQualityDecrease = true;
    parameters.CleanModel = true;
    return parameters;
}

struct Accumulator {
    // 保护 stats。meshCalls 和 doneFaces 各自原子更新。
    std::mutex mutex;
    MeshStats stats;
    std::atomic<std::uint32_t> meshCalls{0};
    std::atomic<std::uint64_t> doneFaces{0};
};

struct MeshJob {
    const model::DocumentModel& model;
    const MeshPolicy& policy;
    MeshLevel level = MeshLevel::SchemeB;
    const std::function<void(model::PrototypeId)>& probe;
    const std::function<void(MeshBatch&&)>& onBatch;
    ProgressSink& progress;
    const CancelToken& cancel;
    ExclusiveSet& exclusive;
    Accumulator& accumulator;
    std::uint64_t totalFaces = 0;
};

void meshOne(MeshJob& job, std::size_t prototypeIndex) {
    const model::PrototypeInfo& info = job.model.prototypes()[prototypeIndex];
    ExclusiveLock exclusive(job.exclusive, info.id);
    job.cancel.throwIfCancelled();
    occt::OcctBoundary::armWorkerThread();

    const MeshParams params =
        job.policy.paramsFor(info, job.model.worldBox(), job.model.lengthUnitToMm(), job.level);
    if (!info.shape.IsNull()) {
        BRepTools::Clean(info.shape);
        const std::uint64_t doneBefore = job.accumulator.doneFaces.load(std::memory_order_relaxed);
        MeshCancelProgress indicator(job.progress, job.cancel, doneBefore, info.faceCount, job.totalFaces);
        const Message_ProgressRange range = indicator.Start();
        // 精度在 BRepMesh 分配节点时已经是双精度。事后 SetDoublePrecision(false) 会抛，
        // 大坐标的单精度误差也会超过 0.001 mm。这里不改 Poly_Triangulation 的精度。
        BRepMesh_IncrementalMesh mesher(info.shape, occtParameters(params), range);
        job.accumulator.meshCalls.fetch_add(1, std::memory_order_relaxed);
        if (job.cancel.isCancelled()) {
            throw CancelledException{};
        }
        if (!static_cast<bool>(mesher.IsDone())) {
            Log::warn(std::format("prototype {} mesher reported not done", info.id));
        }
    }

    if (job.probe) {
        job.probe(info.id);
    }
    job.cancel.throwIfCancelled();

    const FaceMesh faces = summarizeMesh(info.shape);
    if (faces.missing > 0) {
        Log::warn(std::format("prototype {} left {} face(s) without triangulation", info.id, faces.missing));
    }

    MeshBatch batch;
    batch.id = info.id;
    batch.level = job.level;
    batch.triangleCount = faces.triangles;
    batch.approxBytes = faces.bytes;
    if (info.colorGroups.empty()) {
        job.cancel.throwIfCancelled();
        ColorGroupArrays arrays;
        arrays.color = Quantity_ColorRGBA{Quantity_Color{0.7, 0.7, 0.7, Quantity_TOC_RGB}};
        if (!info.shape.IsNull()) {
            arrays.triangles = StdPrs_ShadedShape::FillTriangles(info.shape);
            arrays.edges = StdPrs_ShadedShape::FillFaceBoundaries(info.shape);
        }
        batch.approxBytes += displayBytes(arrays.triangles);
        batch.approxBytes += displayBytes(arrays.edges);
        batch.groups.push_back(std::move(arrays));
    } else {
        batch.groups.reserve(info.colorGroups.size());
        for (const auto& group : info.colorGroups) {
            job.cancel.throwIfCancelled();
            ColorGroupArrays arrays;
            arrays.color = group.first;
            arrays.triangles = StdPrs_ShadedShape::FillTriangles(group.second);
            arrays.edges = StdPrs_ShadedShape::FillFaceBoundaries(group.second);
            batch.approxBytes += displayBytes(arrays.triangles);
            batch.approxBytes += displayBytes(arrays.edges);
            batch.groups.push_back(std::move(arrays));
        }
    }

    {
        std::lock_guard lock(job.accumulator.mutex);
        job.accumulator.stats.triangleCount += batch.triangleCount;
        job.accumulator.stats.instanceTriangleCount +=
            batch.triangleCount * static_cast<std::uint64_t>(info.instanceCount);
        job.accumulator.stats.approxBytes += batch.approxBytes;
        ++job.accumulator.stats.prototypesMeshed;
        job.accumulator.stats.facesWithoutTriangles += faces.missing;
    }
    const std::uint64_t done =
        job.accumulator.doneFaces.fetch_add(info.faceCount, std::memory_order_relaxed) + info.faceCount;
    job.progress.setPresentFaceFraction(portionOf(static_cast<double>(done), job.totalFaces));
    if (job.onBatch) {
        job.onBatch(std::move(batch));
    }
}

} // namespace

Tessellator::Tessellator(TaskScheduler& scheduler, MeshPolicy policy)
    : m_scheduler(scheduler), m_policy(std::move(policy)) {}

void Tessellator::setExclusiveProbe(std::function<void(model::PrototypeId)> probe) {
    m_probe = std::move(probe);
}

Result<MeshStats> Tessellator::run(const model::DocumentModel& model, MeshLevel level,
                                   const std::function<void(MeshBatch&&)>& onBatch, ProgressSink& progress,
                                   const CancelToken& cancel) {
    occt::OcctBoundary::armWorkerThread();
    return occt::catchOcctAsResult(
        [&]() -> MeshStats {
            cancel.throwIfCancelled();
            const std::span<const model::PrototypeInfo> prototypes = model.prototypes();
            std::uint64_t totalFaces = 0;
            for (const model::PrototypeInfo& info : prototypes) {
                totalFaces += info.faceCount;
            }
            progress.setPresentFaceFraction(0.f);

            const std::vector<WorkItem> items = planWork(prototypes, m_policy.largeFaceCount());
            ExclusiveSet exclusive;
            Accumulator accumulator;
            MeshJob job{model,    m_policy, level,     m_probe,     onBatch,
                        progress, cancel,   exclusive, accumulator, totalFaces};

            std::size_t cursor = 0;
            while (cursor < items.size()) {
                cancel.throwIfCancelled();
                if (items[cursor].exclusive) {
                    // 大原型或共享了面/边的一组：留在调用线程上，不和本池的其它任务一起跑。
                    // 面数超过阈值时参数里的 InParallel 才会打开，那时候只用 OCCT 自己的池。
                    for (const std::size_t index : items[cursor].indexes) {
                        meshOne(job, index);
                    }
                    ++cursor;
                    continue;
                }
                std::size_t end = cursor + 1;
                while (end < items.size() && !items[end].exclusive) {
                    ++end;
                }
                const std::size_t begin = cursor;
                // 这一段都是小原型，每项一个原型。InParallel 保持关闭，避免和本池叠出第二套线程。
                m_scheduler.parallelFor(
                    end - begin,
                    [&](std::size_t local) {
                        for (const std::size_t index : items[begin + local].indexes) {
                            meshOne(job, index);
                        }
                    },
                    cancel, 1);
                cursor = end;
            }

            progress.setPresentFaceFraction(1.f);
            accumulator.stats.meshCalls = accumulator.meshCalls.load(std::memory_order_relaxed);
            return accumulator.stats;
        },
        ErrorCode::MeshFailed, "生成网格时出错");
}

} // namespace steplens::mesh
