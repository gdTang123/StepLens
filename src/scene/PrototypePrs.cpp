#include "scene/PrototypePrs.h"

#include <Aspect_TypeOfLine.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <PrsMgr_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Select3D_SensitivePrimitiveArray.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <SelectMgr_Selection.hxx>

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
    // 模式 0 是整体。1 顶点、2 边、4 面留给 T12，这里不建子形状敏感体。
    if (theMode != 0 || theSelection.IsNull()) {
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
