#include "scene/PrototypePrs.h"

#include <Aspect_TypeOfLine.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <PrsMgr_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Select3D_SensitivePoint.hxx>
#include <Select3D_SensitivePrimitiveArray.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <SelectMgr_Selection.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <StdSelect_BRepSelectionTool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <cmath>
#include <type_traits>

namespace steplens::scene {
namespace {

// 每个三角形一个 BVH 节点会把大网格的选择构建拖进几十毫秒以上。
// 合成块只影响遍历，不改变拾取到的是哪一个实例。
constexpr int kSelectionPatch = 32;

constexpr float kOpaqueAlpha = 1.0F;
constexpr double kEdgeWidth = 1.0;

} // namespace

PrototypePrs::PrototypePrs(mesh::MeshBatch batch, TopoDS_Shape shape)
    : AIS_InteractiveObject(PrsMgr_TOP_AllView), m_batch(std::move(batch)), m_shape(std::move(shape)) {}

int PrototypePrs::presentationGroupCount() {
    int groups = 0;
    auto& presentations = Presentations();
    for (std::remove_reference_t<decltype(presentations)>::Iterator it(presentations); it.More(); it.Next()) {
        const auto& presentation = it.Value();
        if (!presentation.IsNull()) {
            groups += presentation->NumberOfGroups();
        }
    }
    return groups;
}

bool PrototypePrs::AcceptDisplayMode(const int theMode) const {
    return theMode == Shaded || theMode == ShadedWithEdges || theMode == Wireframe;
}

Standard_Boolean PrototypePrs::AcceptShapeDecomposition() const {
    return Standard_True;
}

void PrototypePrs::Compute(const opencascade::handle<PrsMgr_PresentationManager>&,
                           const opencascade::handle<Prs3d_Presentation>& thePrs, const int theMode) {
    const bool shaded = theMode == Shaded || theMode == ShadedWithEdges;
    const bool edges = theMode == ShadedWithEdges || theMode == Wireframe;
    if (thePrs.IsNull() || (!shaded && !edges)) {
        return;
    }

    for (const mesh::ColorGroupArrays& group : m_batch.groups) {
        if (shaded && !group.triangles.IsNull()) {
            const opencascade::handle<Graphic3d_Group> shadedGroup = thePrs->NewGroup();
            opencascade::handle<Prs3d_ShadingAspect> aspect = new Prs3d_ShadingAspect();
            aspect->SetColor(group.color.GetRGB());
            const float alpha = group.color.Alpha();
            if (alpha < kOpaqueAlpha) {
                aspect->SetTransparency(static_cast<double>(kOpaqueAlpha - alpha));
            }
            shadedGroup->SetGroupPrimitivesAspect(aspect->Aspect());
            shadedGroup->AddPrimitiveArray(group.triangles);
        }
        if (edges && !group.edges.IsNull()) {
            const opencascade::handle<Graphic3d_Group> edgeGroup = thePrs->NewGroup();
            opencascade::handle<Graphic3d_AspectLine3d> line =
                new Graphic3d_AspectLine3d(group.color.GetRGB(), Aspect_TOL_SOLID, kEdgeWidth);
            edgeGroup->SetGroupPrimitivesAspect(line);
            edgeGroup->AddPrimitiveArray(group.edges);
        }
    }
}

void PrototypePrs::ComputeSelection(const opencascade::handle<SelectMgr_Selection>& theSelection,
                                    const int theMode) {
    if (theSelection.IsNull()) {
        return;
    }
    // 模式 1 手写顶点。模式 2、4 交给 OCCT 的边/面选择工具，不改距离算法。
    if (theMode == 1) {
        if (m_shape.IsNull()) {
            return;
        }
        for (TopExp_Explorer explorer(m_shape, TopAbs_VERTEX); explorer.More(); explorer.Next()) {
            const TopoDS_Vertex vertex = TopoDS::Vertex(explorer.Current());
            const gp_Pnt point = BRep_Tool::Pnt(vertex);
            if (!std::isfinite(point.X()) || !std::isfinite(point.Y()) || !std::isfinite(point.Z())) {
                continue;
            }
            opencascade::handle<StdSelect_BRepOwner> owner = new StdSelect_BRepOwner(vertex, this, 5, true);
            opencascade::handle<Select3D_SensitivePoint> sensitive =
                new Select3D_SensitivePoint(owner, point);
            theSelection->Add(sensitive);
        }
        return;
    }
    if (theMode == 2 || theMode == 4) {
        if (m_shape.IsNull()) {
            return;
        }
        const opencascade::handle<Prs3d_Drawer> drawer = Attributes();
        const double deflection = drawer.IsNull() || !(drawer->DeviationCoefficient() > 0.0)
                                      ? 0.001
                                      : drawer->DeviationCoefficient();
        const double angle =
            drawer.IsNull() || !(drawer->DeviationAngle() > 0.0) ? 0.5 : drawer->DeviationAngle();
        StdSelect_BRepSelectionTool::Load(theSelection, this, m_shape,
                                          theMode == 2 ? TopAbs_EDGE : TopAbs_FACE, deflection, angle);
        return;
    }
    if (theMode != 0) {
        return;
    }
    opencascade::handle<SelectMgr_EntityOwner> owner = new SelectMgr_EntityOwner(this);
    for (const mesh::ColorGroupArrays& group : m_batch.groups) {
        if (group.triangles.IsNull()) {
            continue;
        }
        opencascade::handle<Select3D_SensitivePrimitiveArray> sensitive =
            new Select3D_SensitivePrimitiveArray(owner);
        sensitive->SetPatchSizeMax(kSelectionPatch);
        if (!sensitive->InitTriangulation(group.triangles->Attributes(), group.triangles->Indices(),
                                          TopLoc_Location())) {
            continue;
        }
        theSelection->Add(sensitive);
    }
}

} // namespace steplens::scene
