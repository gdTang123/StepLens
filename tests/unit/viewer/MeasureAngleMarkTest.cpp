#include "measure/DistanceMeasure.h"
#include "viewer/MeasureAngleMark.h"
#include "viewer/MeasureTextLabel.h"

#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Graphic3d_AspectText3d.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Prs3d_TextAspect.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <gtest/gtest.h>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;

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

double arcSpanDegreesAtHinge(const steplens::AngleMarkPlacement& placement) {
    EXPECT_FALSE(placement.arc.IsNull());
    EXPECT_EQ(placement.arc.ShapeType(), TopAbs_EDGE);
    if (placement.arc.IsNull() || placement.arc.ShapeType() != TopAbs_EDGE) {
        return -1.0;
    }
    BRepAdaptor_Curve curve(TopoDS::Edge(placement.arc));
    const gp_Pnt p0 = curve.Value(curve.FirstParameter());
    const gp_Pnt p1 = curve.Value(curve.LastParameter());
    gp_Vec v0(placement.hinge, p0);
    gp_Vec v1(placement.hinge, p1);
    EXPECT_GT(v0.SquareMagnitude(), 1.0e-18);
    EXPECT_GT(v1.SquareMagnitude(), 1.0e-18);
    return v0.Angle(v1) * kDeg;
}

steplens::measure::MeasureShape measureOf(const TopoDS_Shape& shape) {
    steplens::measure::MeasureShape item;
    item.shape = shape;
    return item;
}

} // namespace

TEST(MeasureAngleMark, OrthogonalBoxFacesAnchorOutsideAndBuildArc) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
    const TopoDS_Face faceX = faceWithNormalNear(box, gp_Dir(1.0, 0.0, 0.0));
    const TopoDS_Face faceY = faceWithNormalNear(box, gp_Dir(0.0, 1.0, 0.0));
    ASSERT_FALSE(faceX.IsNull());
    ASSERT_FALSE(faceY.IsNull());

    const steplens::AngleMarkPlacement placement =
        steplens::computeAngleMarkPlacement(measureOf(faceX), measureOf(faceY));
    ASSERT_TRUE(placement.valid);
    ASSERT_FALSE(placement.arc.IsNull());
    // 交线约在 (40,30,*)，外侧沿 +X/+Y，文字应落在实体外。
    EXPECT_GT(placement.labelAnchor.X(), 40.0 - 1.0e-6);
    EXPECT_GT(placement.labelAnchor.Y(), 30.0 - 1.0e-6);
    EXPECT_FALSE(placement.labelAnchor.IsEqual(placement.hinge, 1.0e-6));
    EXPECT_NEAR(arcSpanDegreesAtHinge(placement), 90.0, 0.5);
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

TEST(MeasureAngleMark, ReversedBoxFacesAnchorOutside) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
    const TopoDS_Face faceNegX = faceWithNormalNear(box, gp_Dir(-1.0, 0.0, 0.0));
    const TopoDS_Face faceNegY = faceWithNormalNear(box, gp_Dir(0.0, -1.0, 0.0));
    ASSERT_FALSE(faceNegX.IsNull());
    ASSERT_FALSE(faceNegY.IsNull());
    EXPECT_EQ(faceNegX.Orientation(), TopAbs_REVERSED);
    EXPECT_EQ(faceNegY.Orientation(), TopAbs_REVERSED);

    const steplens::AngleMarkPlacement placement =
        steplens::computeAngleMarkPlacement(measureOf(faceNegX), measureOf(faceNegY));
    ASSERT_TRUE(placement.valid);
    ASSERT_FALSE(placement.arc.IsNull());
    EXPECT_LT(placement.labelAnchor.X(), 0.0);
    EXPECT_LT(placement.labelAnchor.Y(), 0.0);
    EXPECT_NEAR(arcSpanDegreesAtHinge(placement), 90.0, 0.5);
}

TEST(MeasureAngleMark, NonOrthogonalPlanesArcSpansThirtyDegrees) {
    const gp_Pln planeA(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    const double thirty = kPi / 6.0;
    const gp_Pln planeB(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, -std::sin(thirty), std::cos(thirty)));
    BRepBuilderAPI_MakeFace makeA(planeA, -20.0, 20.0, -20.0, 20.0);
    BRepBuilderAPI_MakeFace makeB(planeB, -20.0, 20.0, -20.0, 20.0);
    ASSERT_TRUE(makeA.IsDone());
    ASSERT_TRUE(makeB.IsDone());

    const steplens::AngleMarkPlacement placement =
        steplens::computeAngleMarkPlacement(measureOf(makeA.Face()), measureOf(makeB.Face()));
    ASSERT_TRUE(placement.valid);
    ASSERT_FALSE(placement.arc.IsNull());
    const double span = arcSpanDegreesAtHinge(placement);
    EXPECT_NEAR(span, 30.0, 0.5);
    EXPECT_GT(std::abs(span - 150.0), 50.0); // 绝不是 150°
}

TEST(MeasureAngleMark, EdgeFaceParallelHasNoArc) {
    const gp_Pln plane(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    BRepBuilderAPI_MakeFace makeFace(plane, -10.0, 10.0, -10.0, 10.0);
    ASSERT_TRUE(makeFace.IsDone());
    BRepBuilderAPI_MakeEdge makeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(5.0, 0.0, 0.0));
    ASSERT_TRUE(makeEdge.IsDone());

    steplens::AngleMarkPlacement placement;
    EXPECT_NO_THROW({
        placement =
            steplens::computeAngleMarkPlacement(measureOf(makeEdge.Edge()), measureOf(makeFace.Face()));
    });
    ASSERT_TRUE(placement.valid);
    EXPECT_TRUE(placement.arc.IsNull());
}

TEST(MeasureAngleMark, EdgeFacePerpendicularArcSpansNinetyDegrees) {
    const gp_Pln plane(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    BRepBuilderAPI_MakeFace makeFace(plane, -10.0, 10.0, -10.0, 10.0);
    ASSERT_TRUE(makeFace.IsDone());
    BRepBuilderAPI_MakeEdge makeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(0.0, 0.0, 5.0));
    ASSERT_TRUE(makeEdge.IsDone());

    const steplens::AngleMarkPlacement placement =
        steplens::computeAngleMarkPlacement(measureOf(makeEdge.Edge()), measureOf(makeFace.Face()));
    ASSERT_TRUE(placement.valid);
    ASSERT_FALSE(placement.arc.IsNull());
    EXPECT_NEAR(arcSpanDegreesAtHinge(placement), 90.0, 0.5);
}

TEST(MeasureAngleMark, EdgeFaceObliqueThirtyDegreeArc) {
    const gp_Pln plane(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    BRepBuilderAPI_MakeFace makeFace(plane, -20.0, 20.0, -20.0, 20.0);
    ASSERT_TRUE(makeFace.IsDone());
    // 边与平面夹角 30°：方向 (cos30, 0, sin30)，|d·n|=sin30，asin=30°。
    const gp_Dir edgeDir(std::cos(kPi / 6.0), 0.0, std::sin(kPi / 6.0));
    BRepBuilderAPI_MakeEdge makeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(edgeDir.XYZ() * 10.0));
    ASSERT_TRUE(makeEdge.IsDone());

    const steplens::AngleMarkPlacement placement =
        steplens::computeAngleMarkPlacement(measureOf(makeEdge.Edge()), measureOf(makeFace.Face()));
    ASSERT_TRUE(placement.valid);
    ASSERT_FALSE(placement.arc.IsNull());
    const double span = arcSpanDegreesAtHinge(placement);
    EXPECT_NEAR(span, 30.0, 0.5);
    EXPECT_GT(std::abs(span - 60.0), 20.0);
}

TEST(MeasureAngleMark, EdgeEdgeOppositeDirectionsUseAcuteArc) {
    // 两直线夹锐角 40°，但第二边方向取反，点积为负。
    const double forty = 40.0 * kPi / 180.0;
    BRepBuilderAPI_MakeEdge edgeA(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(10.0, 0.0, 0.0));
    BRepBuilderAPI_MakeEdge edgeB(gp_Pnt(0.0, 0.0, 0.0),
                                  gp_Pnt(-10.0 * std::cos(forty), -10.0 * std::sin(forty), 0.0));
    ASSERT_TRUE(edgeA.IsDone());
    ASSERT_TRUE(edgeB.IsDone());

    const steplens::AngleMarkPlacement placement =
        steplens::computeAngleMarkPlacement(measureOf(edgeA.Edge()), measureOf(edgeB.Edge()));
    ASSERT_TRUE(placement.valid);
    ASSERT_FALSE(placement.arc.IsNull());
    const double span = arcSpanDegreesAtHinge(placement);
    EXPECT_NEAR(span, 40.0, 0.5);
    EXPECT_LT(span, 90.0);
    EXPECT_GT(std::abs(span - 140.0), 50.0);
}

TEST(MeasureAngleMark, DegenerateOrEmptyShapesDoNotThrow) {
    steplens::AngleMarkPlacement empty;
    EXPECT_NO_THROW({ empty = steplens::computeAngleMarkPlacement({}, {}); });
    EXPECT_FALSE(empty.valid);

    TopoDS_Edge degenerated;
    BRep_Builder builder;
    builder.MakeEdge(degenerated);
    builder.Degenerated(degenerated, true);
    ASSERT_TRUE(BRep_Tool::Degenerated(degenerated));

    const gp_Pln plane(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    BRepBuilderAPI_MakeFace makeFace(plane, -5.0, 5.0, -5.0, 5.0);
    ASSERT_TRUE(makeFace.IsDone());

    steplens::AngleMarkPlacement fromDegenerate;
    EXPECT_NO_THROW({
        fromDegenerate =
            steplens::computeAngleMarkPlacement(measureOf(degenerated), measureOf(makeFace.Face()));
    });
    // 退化边无有效几何：安全返回，不抛。
    EXPECT_TRUE(!fromDegenerate.valid || fromDegenerate.arc.IsNull());

    steplens::measure::MeasureShape nullShape;
    steplens::AngleMarkPlacement fromNull;
    EXPECT_NO_THROW(
        { fromNull = steplens::computeAngleMarkPlacement(nullShape, measureOf(makeFace.Face())); });
    EXPECT_FALSE(fromNull.valid);
}
