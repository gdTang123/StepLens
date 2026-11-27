#include "measure/DistanceMeasure.h"
#include "viewer/MeasureAngleMark.h"
#include "viewer/MeasureTextLabel.h"

#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Graphic3d_AspectText3d.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Prs3d_TextAspect.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <gtest/gtest.h>
#include <vector>

namespace {

TopoDS_Face faceWithNormalNear(const TopoDS_Shape& box, const gp_Dir& wanted) {
    TopoDS_Face best;
    double bestDot = -2.0;
    for (TopExp_Explorer faces(box, TopAbs_FACE); faces.More(); faces.Next()) {
        const TopoDS_Face face = TopoDS::Face(faces.Current());
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        gp_Dir normal = surface.Plane().Axis().Direction();
        if (face.Orientation() == TopAbs_REVERSED) {
            normal.Reverse();
        }
        const double dot = normal.Dot(wanted);
        if (dot > bestDot) {
            bestDot = dot;
            best = face;
        }
    }
    return best;
}

} // namespace

TEST(MeasureAngleMark, OrthogonalBoxFacesAnchorOutsideAndBuildArc) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
    const TopoDS_Face faceX = faceWithNormalNear(box, gp_Dir(1.0, 0.0, 0.0));
    const TopoDS_Face faceY = faceWithNormalNear(box, gp_Dir(0.0, 1.0, 0.0));
    ASSERT_FALSE(faceX.IsNull());
    ASSERT_FALSE(faceY.IsNull());

    steplens::measure::MeasureShape a;
    a.shape = faceX;
    steplens::measure::MeasureShape b;
    b.shape = faceY;

    const steplens::AngleMarkPlacement placement = steplens::computeAngleMarkPlacement(a, b);
    ASSERT_TRUE(placement.valid);
    ASSERT_FALSE(placement.arc.IsNull());
    // 交线约在 (40,30,*)，外侧沿 +X/+Y，文字应落在实体外。
    EXPECT_GT(placement.labelAnchor.X(), 40.0 - 1.0e-6);
    EXPECT_GT(placement.labelAnchor.Y(), 30.0 - 1.0e-6);
    EXPECT_FALSE(placement.labelAnchor.IsEqual(placement.hinge, 1.0e-6));
}

TEST(MeasureAngleMark, OverlayUsesTopmostLayerAndKeepsTextConfig) {
    opencascade::handle<AIS_TextLabel> label = new AIS_TextLabel();
    label->SetText(TCollection_ExtendedString("90.00°"));
    label->SetPosition(gp_Pnt(41.0, 31.0, 10.0));
    steplens::configureMeasureTextLabel(label);
    steplens::configureMeasureOverlayObject(label);
    EXPECT_EQ(label->ZLayer(), Graphic3d_ZLayerId_Topmost);
    EXPECT_FALSE(label->Attributes()->TextAspect()->Aspect()->GetTextZoomable());
    EXPECT_NEAR(label->Attributes()->TextAspect()->Height(), steplens::kMeasureLabelPointSize, 1.0e-9);

    opencascade::handle<AIS_Shape> arc = new AIS_Shape(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape());
    steplens::configureMeasureOverlayObject(arc);
    EXPECT_EQ(arc->ZLayer(), Graphic3d_ZLayerId_Topmost);
}
