#pragma once

// T07 单级 Tessellator，方案 B（c = 0.002）。
// 每个原型只三角化一次，实例共用面上的网格。按面数 × 实例数从大到小调度。
// 面数超过 MeshPolicy::largeFaceCount 的原型独占调用线程，OCCT 自己的线程池按面并行，
// 不再和本池的其它原型任务叠在一起。
// 同一原型上的 FillTriangles / FillFaceBoundaries 只在这条独占路径里调用。
// Poly_Triangulation 保持 BRepMesh 写下来的双精度，不在网格生成之后改单精度。
// T07+ 的粗/细两级、T08 的 SceneBuilder / AIS 不在这里。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "core/Result.h"
#include "core/TaskScheduler.h"
#include "mesh/MeshPolicy.h"

#include <functional>

namespace steplens::mesh {

class Tessellator {
public:
    // scheduler 必须是 occt::makeOcctTaskScheduler 创建的。run 会在调用线程和池线程上跑 OCCT。
    // 不要在这个池的工作线程里调用 run：parallelFor 不允许重入。
    Tessellator(TaskScheduler& scheduler, MeshPolicy policy);

    Tessellator(const Tessellator&) = delete;
    Tessellator& operator=(const Tessellator&) = delete;
    Tessellator(Tessellator&&) = delete;
    Tessellator& operator=(Tessellator&&) = delete;

    // 测试缝。位于某个原型的独占区里：IncrementalMesh 之后、FillTriangles 之前。
    // 不同原型可能并发进入；同一个原型不会。回调要自己同步。
    // 不要在回调里再调用 run。生产路径不设置。
    void setExclusiveProbe(std::function<void(model::PrototypeId)> probe);

    // onBatch 在算出该原型的工作线程上调用，调用后本对象不再改这份 MeshBatch。
    // run 会把回调串行化：多个原型同时网格化时，回调本身不会并发。
    // 取消时已交付的批次仍然有效，返回值是 Cancelled。
    // 不调用 ProgressSink::reset，也不改 LoadStage。进度只用 setPresentFaceFraction。
    [[nodiscard]] Result<MeshStats> run(const model::DocumentModel& model, MeshLevel level,
                                        const std::function<void(MeshBatch&&)>& onBatch,
                                        ProgressSink& progress, const CancelToken& cancel);

private:
    TaskScheduler& m_scheduler;
    MeshPolicy m_policy;
    std::function<void(model::PrototypeId)> m_probe;
};

} // namespace steplens::mesh
