#pragma once

// T08 原型显示对象。Compute() 只把工作线程已经填好的数组挂到 Group 上。
// 这个对象本身不进交互上下文。实例用 AIS_ConnectedInteractive 引用它。
// replaceMesh 是 T08+，这里没有。顶点/边/面选择是 T12，这里只做整体选择。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "mesh/MeshBatch.h"

#include <AIS_InteractiveObject.hxx>
#include <TopoDS_Shape.hxx>

namespace steplens::scene {

class PrototypePrs final : public AIS_InteractiveObject {
public:
    // 和 OCCT 的 AIS_WireFrame / AIS_Shaded 编号不是一回事。Display 时把这个值传给 Compute。
    enum Mode { Shaded = 0, ShadedWithEdges = 1, Wireframe = 2 };

    PrototypePrs(mesh::MeshBatch batch, TopoDS_Shape shape);

    [[nodiscard]] const mesh::MeshBatch& mesh() const {
        return m_batch;
    }
    // 选择和以后的测量用原型形状。实例变换不在这里。
    [[nodiscard]] const TopoDS_Shape& shape() const {
        return m_shape;
    }
    // 各显示模式里已经挂上的 Group 总数。还没算过就是 0。
    // HasPresentation() 只看当前 DisplayMode，原型本身不 Display，那个值对不上。
    [[nodiscard]] int presentationGroupCount();

    bool AcceptDisplayMode(const int theMode) const override;

    DEFINE_STANDARD_RTTI_INLINE(PrototypePrs, AIS_InteractiveObject)

protected:
    void Compute(const opencascade::handle<PrsMgr_PresentationManager>& thePrsMgr,
                 const opencascade::handle<Prs3d_Presentation>& thePrs, const int theMode) override;
    void ComputeSelection(const opencascade::handle<SelectMgr_Selection>& theSelection,
                          const int theMode) override;

private:
    mesh::MeshBatch m_batch;
    TopoDS_Shape m_shape;
};

} // namespace steplens::scene
