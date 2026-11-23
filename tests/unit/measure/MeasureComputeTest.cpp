#include "measure/AngleMeasure.h"
#include "measure/DistanceMeasure.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Geom_Line.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <gtest/gtest.h>
#include <limits>

namespace {
using steplens::ErrorCode;
using steplens::measure::MeasureShape;

MeasureShape vertex(double x, double y = 0, double z = 0) {
    return {BRepBuilderAPI_MakeVertex(gp_Pnt(x, y, z)).Shape(), gp_Trsf{}};
}

MeasureShape edge(const gp_Pnt& p1, const gp_Pnt& p2) {
    return {BRepBuilderAPI_MakeEdge(p1, p2).Shape(), gp_Trsf{}};
}

MeasureShape planarFace(const gp_Pln& plane, double umin, double umax, double vmin, double vmax) {
    return {BRepBuilderAPI_MakeFace(plane, umin, umax, vmin, vmax).Shape(), gp_Trsf{}};
}

MeasureShape zeroLengthEdge() {
    opencascade::handle<Geom_Line> line = new Geom_Line(gp_Lin(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    TopoDS_Edge edge;
    BRep_Builder builder;
    builder.MakeEdge(edge, line, Precision::Confusion());
    builder.Range(edge, 0.0, 0.0);
    return {edge, gp_Trsf{}};
}

MeasureShape degeneratedEdgeWithVertex() {
    BRep_Builder builder;
    TopoDS_Vertex origin;
    builder.MakeVertex(origin, gp_Pnt(0, 0, 0), Precision::Confusion());
    TopoDS_Edge edge;
    builder.MakeEdge(edge);
    TopoDS_Vertex forward = origin;
    TopoDS_Vertex reversed = origin;
    forward.Orientation(TopAbs_FORWARD);
    reversed.Orientation(TopAbs_REVERSED);
    builder.Add(edge, forward);
    builder.Add(edge, reversed);
    builder.Degenerated(edge, Standard_True);
    return {edge, gp_Trsf{}};
}

MeasureShape cylindricalFace() {
    BRepPrimAPI_MakeCylinder cylinder(2.0, 10.0);
    for (TopExp_Explorer explorer(cylinder.Shape(), TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face face = TopoDS::Face(explorer.Current());
        if (BRepAdaptor_Surface(face).GetType() == GeomAbs_Cylinder) {
            return {face, gp_Trsf{}};
        }
    }
    return {};
}

MeasureShape circularEdge() {
    const gp_Circ circle(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5.0);
    return {BRepBuilderAPI_MakeEdge(circle, 0.0, 1.5707963267948966).Shape(), gp_Trsf{}};
}

MeasureShape straightEdge(const gp_Pnt& start, const gp_Dir& dir, double length) {
    opencascade::handle<Geom_Line> line = new Geom_Line(gp_Lin(start, dir));
    BRep_Builder builder;
    TopoDS_Edge edge;
    builder.MakeEdge(edge, line, 1e-12);
    builder.Range(edge, 0.0, length);
    TopoDS_Vertex v0;
    TopoDS_Vertex v1;
    builder.MakeVertex(v0, start, 1e-12);
    builder.MakeVertex(v1, gp_Pnt(start.XYZ() + dir.XYZ() * length), 1e-12);
    v0.Orientation(TopAbs_FORWARD);
    v1.Orientation(TopAbs_REVERSED);
    builder.Add(edge, v0);
    builder.Add(edge, v1);
    return {edge, gp_Trsf{}};
}

TEST(MeasureCompute, PointDistanceIsMillimeters) {
    auto r = steplens::measure::distanceBetween(vertex(0), vertex(10), 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 10.0, 0.001);
}
TEST(MeasureCompute, InstanceTransformPreservesDistanceAndMovesEndpoints) {
    auto a = vertex(0), b = vertex(10);
    gp_Trsf move;
    move.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267948966);
    move.SetTranslationPart(gp_Vec(5, 7, 0));
    a.transform = move;
    b.transform = move;
    auto r = steplens::measure::distanceBetween(a, b, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 10.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(5, 7, 0), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(5, 17, 0), 0.001));
}
TEST(MeasureCompute, ExistingShapeLocationAndInstanceTransformAreEachAppliedOnce) {
    auto a = vertex(0);
    gp_Trsf local;
    local.SetTranslation(gp_Vec(10, 0, 0));
    a.shape = a.shape.Moved(TopLoc_Location(local));
    gp_Trsf instance;
    instance.SetTranslation(gp_Vec(100, 0, 0));
    a.transform = instance;
    auto r = steplens::measure::distanceBetween(a, vertex(111), 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 1.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(110, 0, 0), 0.001));
}
TEST(MeasureCompute, EmptyShapeAndInvalidUnitsReportDistinctErrors) {
    MeasureShape empty{};
    auto g = steplens::measure::distanceBetween(empty, vertex(0), 1.0);
    ASSERT_FALSE(g.ok());
    EXPECT_NE(g.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    for (double u :
         {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto r = steplens::measure::distanceBetween(vertex(0), vertex(1), u);
        ASSERT_FALSE(r.ok());
        EXPECT_NE(r.error().userMessage.find("长度单位无效"), std::string::npos);
    }
}
TEST(MeasureCompute, UnitScaleIsAppliedOnceToDistanceOnly) {
    auto r = steplens::measure::distanceBetween(vertex(0), vertex(2), 1000.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 2000.0, 0.001);
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(2, 0, 0), 0.001));
}

TEST(MeasureCompute, PointEdgeDistanceIsMillimeters) {
    auto r =
        steplens::measure::distanceBetween(vertex(0, 0, 0), edge(gp_Pnt(0, 5, 0), gp_Pnt(10, 5, 0)), 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 5.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(0, 0, 0), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(0, 5, 0), 0.001));
}

TEST(MeasureCompute, PointFaceDistanceIsMillimeters) {
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 20);
    auto r = steplens::measure::distanceBetween(vertex(3, 4, 7), face, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 7.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(3, 4, 7), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(3, 4, 0), 0.001));
}

TEST(MeasureCompute, EdgeEdgeDistanceIsMillimeters) {
    auto r = steplens::measure::distanceBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
                                                edge(gp_Pnt(4, 6, 0), gp_Pnt(4, 6, 8)), 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 6.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(4, 0, 0), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(4, 6, 0), 0.001));
}

TEST(MeasureCompute, ParallelPlaneDistanceIsMillimeters) {
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 30), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::distanceBetween(a, b, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 30.0, 0.001);
    EXPECT_NEAR(r.value().pointA.Z(), 0.0, 0.001);
    EXPECT_NEAR(r.value().pointB.Z(), 30.0, 0.001);
    EXPECT_NEAR(r.value().pointA.X(), r.value().pointB.X(), 0.001);
    EXPECT_NEAR(r.value().pointA.Y(), r.value().pointB.Y(), 0.001);
}

TEST(MeasureCompute, OppositeNormalParallelPlanesAreSupported) {
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 30), gp_Dir(0, 0, -1)), 0, 10, 0, 10);
    auto r = steplens::measure::distanceBetween(a, b, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 30.0, 0.001);
}

TEST(MeasureCompute, NonParallelPlanePairIsUnsupportedCombination) {
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 0, 10, 0, 10);
    auto r = steplens::measure::distanceBetween(a, b, 1.0);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::UnsupportedCombination);
    EXPECT_EQ(r.error().userMessage, "不支持该组合");
}

TEST(MeasureCompute, NonPlanarFacePairIsUnsupportedCombination) {
    const auto cylinder = cylindricalFace();
    ASSERT_FALSE(cylinder.shape.IsNull());
    const auto plane = planarFace(gp_Pln(gp_Pnt(10, 0, 0), gp_Dir(1, 0, 0)), -5, 5, -5, 15);
    auto r = steplens::measure::distanceBetween(cylinder, plane, 1.0);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::UnsupportedCombination);
    EXPECT_EQ(r.error().userMessage, "不支持该组合");
}

TEST(MeasureCompute, PointEdgeInstanceTransformPreservesDistanceAndMovesEndpoints) {
    auto a = vertex(0, 0, 0);
    auto b = edge(gp_Pnt(0, 5, 0), gp_Pnt(10, 5, 0));
    gp_Trsf move;
    move.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267948966);
    move.SetTranslationPart(gp_Vec(5, 7, 0));
    a.transform = move;
    b.transform = move;
    auto r = steplens::measure::distanceBetween(a, b, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 5.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(5, 7, 0), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(0, 7, 0), 0.001));
}

TEST(MeasureCompute, ExistingShapeLocationAndInstanceTransformAreEachAppliedOnceOnEdge) {
    auto e = edge(gp_Pnt(0, 5, 0), gp_Pnt(10, 5, 0));
    gp_Trsf local;
    local.SetTranslation(gp_Vec(10, 0, 0));
    e.shape = e.shape.Moved(TopLoc_Location(local));
    gp_Trsf instance;
    instance.SetTranslation(gp_Vec(100, 0, 0));
    e.transform = instance;
    auto r = steplens::measure::distanceBetween(vertex(111, 0, 0), e, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 5.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(111, 0, 0), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(111, 5, 0), 0.001));
}

TEST(MeasureCompute, UnitScaleIsAppliedOnceToPointFaceDistanceOnly) {
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 20);
    auto r = steplens::measure::distanceBetween(vertex(3, 4, 7), face, 1000.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 7000.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(3, 4, 7), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(3, 4, 0), 0.001));
}

TEST(MeasureCompute, ZeroLengthEdgeIsDegenerateGeometry) {
    const auto degenerate = zeroLengthEdge();
    ASSERT_FALSE(degenerate.shape.IsNull());
    auto r = steplens::measure::distanceBetween(vertex(10, 0, 0), degenerate, 1.0);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, ScaledInstanceTransformReturnsControlledError) {
    auto a = vertex(0);
    auto b = vertex(10);
    gp_Trsf scale;
    scale.SetScale(gp_Pnt(0, 0, 0), 2.0);
    a.transform = scale;
    steplens::Result<steplens::measure::DistanceValue> result{
        steplens::Error{ErrorCode::Internal, "uninitialized", "未初始化"}};
    EXPECT_NO_THROW({ result = steplens::measure::distanceBetween(a, b, 1.0); });
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.error().userMessage.empty());
}

TEST(MeasureCompute, MirroredInstanceTransformReturnsControlledError) {
    auto a = vertex(0);
    auto b = vertex(10);
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    a.transform = mirror;
    steplens::Result<steplens::measure::DistanceValue> result{
        steplens::Error{ErrorCode::Internal, "uninitialized", "未初始化"}};
    EXPECT_NO_THROW({ result = steplens::measure::distanceBetween(a, b, 1.0); });
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.error().userMessage.empty());
}

TEST(MeasureCompute, UnitScaleOverflowIsControlledError) {
    auto r = steplens::measure::distanceBetween(vertex(0), vertex(2), std::numeric_limits<double>::max());
    ASSERT_FALSE(r.ok());
    EXPECT_FALSE(r.error().userMessage.empty());
}

TEST(MeasureCompute, NonFiniteInstanceTranslationReturnsControlledError) {
    auto a = vertex(0);
    auto b = vertex(1);
    gp_Trsf move;
    move.SetTranslation(gp_Vec(std::numeric_limits<double>::quiet_NaN(), 0, 0));
    a.transform = move;
    steplens::Result<steplens::measure::DistanceValue> result{
        steplens::Error{ErrorCode::Internal, "uninitialized", "未初始化"}};
    EXPECT_NO_THROW({ result = steplens::measure::distanceBetween(a, b, 1.0); });
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.error().userMessage.empty());
}

TEST(MeasureCompute, DegeneratedEdgeWithVertexAgainstPointIsDegenerateGeometry) {
    const auto degenerate = degeneratedEdgeWithVertex();
    ASSERT_FALSE(degenerate.shape.IsNull());
    ASSERT_EQ(degenerate.shape.ShapeType(), TopAbs_EDGE);
    ASSERT_TRUE(BRep_Tool::Degenerated(TopoDS::Edge(degenerate.shape)));
    auto r = steplens::measure::distanceBetween(vertex(10, 0, 0), degenerate, 1.0);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, DegeneratedEdgeWithVertexAgainstEdgeIsDegenerateGeometry) {
    const auto degenerate = degeneratedEdgeWithVertex();
    ASSERT_FALSE(degenerate.shape.IsNull());
    ASSERT_TRUE(BRep_Tool::Degenerated(TopoDS::Edge(degenerate.shape)));
    auto r = steplens::measure::distanceBetween(degenerate, edge(gp_Pnt(0, 5, 0), gp_Pnt(10, 5, 0)), 1.0);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, StraightEdgePairAngleIs90Degrees) {
    auto r = steplens::measure::angleBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
                                             edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0)));
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 90.0, 0.01);
}

TEST(MeasureCompute, StraightEdgePairAngleIs0DegreesWhenParallel) {
    auto r = steplens::measure::angleBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
                                             edge(gp_Pnt(0, 5, 0), gp_Pnt(10, 5, 0)));
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, OppositeOrientedParallelEdgesReport0Degrees) {
    // Prep §6 盒体直边夹角写的是 0°/90°，不是 180°。反向平行边锁定无符号较小角。
    auto r = steplens::measure::angleBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
                                             edge(gp_Pnt(10, 5, 0), gp_Pnt(0, 5, 0)));
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, StraightEdgePairAngleIs45Degrees) {
    auto r = steplens::measure::angleBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
                                             edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 10, 0)));
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 45.0, 0.01);
}

TEST(MeasureCompute, ObtuseStraightEdgePairReportsSmallerAngle) {
    constexpr double radians150 = 150.0 * 3.14159265358979323846 / 180.0;
    auto r = steplens::measure::angleBetween(
        edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
        edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::cos(radians150), 10 * std::sin(radians150), 0)));
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 30.0, 0.01);
}

TEST(MeasureCompute, ObtusePlaneNormalPairReportsSmallerAngle) {
    constexpr double radians120 = 120.0 * 3.14159265358979323846 / 180.0;
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(std::cos(radians120), std::sin(radians120), 0)),
                              0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(a, b);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 60.0, 0.01);
}

TEST(MeasureCompute, ObliqueStraightEdgeToPlaneAngleIs30Degrees) {
    constexpr double radians30 = 30.0 * 3.14159265358979323846 / 180.0;
    const auto direction =
        edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::cos(radians30), 0, 10 * std::sin(radians30)));
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(direction, face);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 30.0, 0.01);
}

// 边方向与面法向夹角 150°。线面角是 asin(|dot|) = 60°，不是相对平面抬高 30° 的那一例。
TEST(MeasureCompute, EdgeDirection150DegreesFromFaceNormalReports60AndIgnoresEdgeReversal) {
    constexpr double radians150 = 150.0 * 3.14159265358979323846 / 180.0;
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto direction = edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::sin(radians150), 0, 10 * std::cos(radians150)));
    auto forward = steplens::measure::angleBetween(direction, face);
    ASSERT_TRUE(forward.ok());
    EXPECT_NEAR(forward.value().degrees, 60.0, 0.01);

    auto geometricReverse =
        edge(gp_Pnt(10 * std::sin(radians150), 0, 10 * std::cos(radians150)), gp_Pnt(0, 0, 0));
    auto reversedEnds = steplens::measure::angleBetween(geometricReverse, face);
    ASSERT_TRUE(reversedEnds.ok());
    EXPECT_NEAR(reversedEnds.value().degrees, forward.value().degrees, 0.01);

    direction.shape = direction.shape.Reversed();
    auto flipped = steplens::measure::angleBetween(direction, face);
    ASSERT_TRUE(flipped.ok());
    EXPECT_NEAR(flipped.value().degrees, forward.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroEdgePairAngleIsInvariantToSelectionOrder) {
    const auto a = edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    const auto b = edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::cos(3.14159265358979323846 / 3.0),
                                                10 * std::sin(3.14159265358979323846 / 3.0), 0));
    auto ab = steplens::measure::angleBetween(a, b);
    auto ba = steplens::measure::angleBetween(b, a);
    ASSERT_TRUE(ab.ok());
    ASSERT_TRUE(ba.ok());
    EXPECT_NEAR(ab.value().degrees, 60.0, 0.01);
    EXPECT_NEAR(ba.value().degrees, ab.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroPlanePairAngleIsInvariantToSelectionOrder) {
    constexpr double radians120 = 120.0 * 3.14159265358979323846 / 180.0;
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(std::cos(radians120), std::sin(radians120), 0)),
                              0, 10, 0, 10);
    auto ab = steplens::measure::angleBetween(a, b);
    auto ba = steplens::measure::angleBetween(b, a);
    ASSERT_TRUE(ab.ok());
    ASSERT_TRUE(ba.ok());
    EXPECT_NEAR(ab.value().degrees, 60.0, 0.01);
    EXPECT_NEAR(ba.value().degrees, ab.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroEdgeFaceAngleIsInvariantToSelectionOrder) {
    constexpr double radians30 = 30.0 * 3.14159265358979323846 / 180.0;
    const auto direction =
        edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::cos(radians30), 0, 10 * std::sin(radians30)));
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto edgeFace = steplens::measure::angleBetween(direction, face);
    auto faceEdge = steplens::measure::angleBetween(face, direction);
    ASSERT_TRUE(edgeFace.ok());
    ASSERT_TRUE(faceEdge.ok());
    EXPECT_NEAR(edgeFace.value().degrees, 30.0, 0.01);
    EXPECT_NEAR(faceEdge.value().degrees, edgeFace.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroEdgePairAngleIsInvariantToEdgeTopologyOrientation) {
    const auto a = edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    const auto b = edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::cos(3.14159265358979323846 / 3.0),
                                                10 * std::sin(3.14159265358979323846 / 3.0), 0));
    auto original = steplens::measure::angleBetween(a, b);
    auto reversed = b;
    reversed.shape = reversed.shape.Reversed();
    auto flipped = steplens::measure::angleBetween(a, reversed);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(flipped.ok());
    EXPECT_NEAR(original.value().degrees, 60.0, 0.01);
    EXPECT_NEAR(flipped.value().degrees, original.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroEdgeFaceAngleIsInvariantToFaceTopologyOrientation) {
    constexpr double radians30 = 30.0 * 3.14159265358979323846 / 180.0;
    const auto direction =
        edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::cos(radians30), 0, 10 * std::sin(radians30)));
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto original = steplens::measure::angleBetween(direction, face);
    auto reversed = face;
    reversed.shape = reversed.shape.Reversed();
    auto flipped = steplens::measure::angleBetween(direction, reversed);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(flipped.ok());
    EXPECT_NEAR(original.value().degrees, 30.0, 0.01);
    EXPECT_NEAR(flipped.value().degrees, original.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroPlanePairAngleIsInvariantToFirstFaceTopologyOrientation) {
    constexpr double radians120 = 120.0 * 3.14159265358979323846 / 180.0;
    auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(std::cos(radians120), std::sin(radians120), 0)),
                              0, 10, 0, 10);
    auto original = steplens::measure::angleBetween(a, b);
    a.shape = a.shape.Reversed();
    auto flipped = steplens::measure::angleBetween(a, b);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(flipped.ok());
    EXPECT_NEAR(original.value().degrees, 60.0, 0.01);
    EXPECT_NEAR(flipped.value().degrees, original.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroPlanePairAngleIsInvariantToSecondFaceTopologyOrientation) {
    constexpr double radians120 = 120.0 * 3.14159265358979323846 / 180.0;
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 0, 10, 0, 10);
    auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(std::cos(radians120), std::sin(radians120), 0)), 0, 10,
                        0, 10);
    auto original = steplens::measure::angleBetween(a, b);
    b.shape = b.shape.Reversed();
    auto flipped = steplens::measure::angleBetween(a, b);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(flipped.ok());
    EXPECT_NEAR(original.value().degrees, 60.0, 0.01);
    EXPECT_NEAR(flipped.value().degrees, original.value().degrees, 0.01);
}

TEST(MeasureCompute, NonZeroEdgeFaceAngleIsInvariantToEdgeTopologyOrientation) {
    constexpr double radians30 = 30.0 * 3.14159265358979323846 / 180.0;
    auto direction = edge(gp_Pnt(0, 0, 0), gp_Pnt(10 * std::cos(radians30), 0, 10 * std::sin(radians30)));
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto original = steplens::measure::angleBetween(direction, face);
    direction.shape = direction.shape.Reversed();
    auto flipped = steplens::measure::angleBetween(direction, face);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(flipped.ok());
    EXPECT_NEAR(original.value().degrees, 30.0, 0.01);
    EXPECT_NEAR(flipped.value().degrees, original.value().degrees, 0.01);
}

TEST(MeasureCompute, EdgeLengthDoesNotChangeRightAngle) {
    auto r = steplens::measure::angleBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(1, 0, 0)),
                                             edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 100, 0)));
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 90.0, 0.01);
}

TEST(MeasureCompute, PlanePairAngleIs90Degrees) {
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(a, b);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 90.0, 0.01);
}

TEST(MeasureCompute, ParallelPlanesReport0Degrees) {
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 30), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(a, b);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, OppositeNormalParallelPlanesReport0Degrees) {
    // Prep §6「盒面夹角 0°/90°」：反向法向的平行面是 0°，不是 180°。
    const auto a = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    const auto b = planarFace(gp_Pln(gp_Pnt(0, 0, 30), gp_Dir(0, 0, -1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(a, b);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, StraightEdgePerpendicularToPlaneIs90Degrees) {
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 10)), face);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 90.0, 0.01);
}

TEST(MeasureCompute, StraightEdgeParallelToPlaneIs0Degrees) {
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)), face);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, AngleInstanceTransformPreservesRightAngle) {
    auto a = edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    auto b = edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0));
    gp_Trsf move;
    move.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267948966);
    move.SetTranslationPart(gp_Vec(5, 7, 0));
    a.transform = move;
    b.transform = move;
    auto r = steplens::measure::angleBetween(a, b);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 90.0, 0.01);
}

TEST(MeasureCompute, ExistingShapeLocationAndInstanceTransformAreEachAppliedOnceForAngle) {
    auto rotated = edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    gp_Trsf local;
    local.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267948966);
    rotated.shape = rotated.shape.Moved(TopLoc_Location(local));
    gp_Trsf instance;
    instance.SetTranslation(gp_Vec(100, 0, 0));
    rotated.transform = instance;
    auto r = steplens::measure::angleBetween(rotated, edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)));
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 90.0, 0.01);
}

TEST(MeasureCompute, CircularEdgeAngleIsUnsupportedCombination) {
    auto r = steplens::measure::angleBetween(circularEdge(), edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::UnsupportedCombination);
    EXPECT_EQ(r.error().userMessage, "不支持该组合");
}

TEST(MeasureCompute, CylindricalFaceAngleIsUnsupportedCombination) {
    const auto cylinder = cylindricalFace();
    ASSERT_FALSE(cylinder.shape.IsNull());
    const auto plane = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(cylinder, plane);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::UnsupportedCombination);
    EXPECT_EQ(r.error().userMessage, "不支持该组合");
}

TEST(MeasureCompute, VertexPairAngleIsUnsupportedCombination) {
    auto r = steplens::measure::angleBetween(vertex(0, 0, 0), vertex(10, 0, 0));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::UnsupportedCombination);
    EXPECT_EQ(r.error().userMessage, "不支持该组合");
}

TEST(MeasureCompute, EmptyShapeAngleIsDegenerateGeometry) {
    MeasureShape empty{};
    auto r = steplens::measure::angleBetween(empty, edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, ZeroLengthEdgeAngleIsDegenerateGeometry) {
    const auto degenerate = zeroLengthEdge();
    ASSERT_FALSE(degenerate.shape.IsNull());
    auto r = steplens::measure::angleBetween(degenerate, edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, ScaledInstanceTransformAngleReturnsControlledError) {
    auto a = edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    auto b = edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0));
    gp_Trsf scale;
    scale.SetScale(gp_Pnt(0, 0, 0), 2.0);
    a.transform = scale;
    steplens::Result<steplens::measure::AngleValue> result{
        steplens::Error{ErrorCode::Internal, "uninitialized", "未初始化"}};
    EXPECT_NO_THROW({ result = steplens::measure::angleBetween(a, b); });
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.error().userMessage.empty());
}

TEST(MeasureCompute, MirroredInstanceTransformAngleReturnsControlledError) {
    auto a = edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    auto b = edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0));
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    a.transform = mirror;
    steplens::Result<steplens::measure::AngleValue> result{
        steplens::Error{ErrorCode::Internal, "uninitialized", "未初始化"}};
    EXPECT_NO_THROW({ result = steplens::measure::angleBetween(a, b); });
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.error().userMessage.empty());
}

TEST(MeasureCompute, DegeneratedEdgeWithVertexAngleIsDegenerateGeometry) {
    const auto degenerate = degeneratedEdgeWithVertex();
    ASSERT_FALSE(degenerate.shape.IsNull());
    ASSERT_EQ(degenerate.shape.ShapeType(), TopAbs_EDGE);
    ASSERT_TRUE(BRep_Tool::Degenerated(TopoDS::Edge(degenerate.shape)));
    auto r = steplens::measure::angleBetween(degenerate, edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, FiniteTrimmedPlaneLateralOffsetUsesBoundaryNotSupportingPlane) {
    // z=0 有限面 [0,10]x[0,10]；点 (15,5,3) 侧向错位。支撑平面距为 3，裁剪 B-Rep 最短距到 (10,5,0)。
    const auto face = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::distanceBetween(vertex(15, 5, 3), face, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 5.8309518948453, 0.001); // sqrt(5^2 + 3^2)
    EXPECT_GT(std::abs(r.value().millimeters - 3.0), 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(15, 5, 3), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(10, 5, 0), 0.001));
}

TEST(MeasureCompute, ZeroWidthPlanarFaceAngleAgainstPlaneIsDegenerateGeometry) {
    const auto degenerate = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 0, 0, 10);
    ASSERT_FALSE(degenerate.shape.IsNull());
    ASSERT_EQ(degenerate.shape.ShapeType(), TopAbs_FACE);
    const auto plane = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(degenerate, plane);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, ZeroWidthPlanarFaceAngleAgainstEdgeIsDegenerateGeometry) {
    const auto degenerate = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 0, 0, 10);
    ASSERT_FALSE(degenerate.shape.IsNull());
    ASSERT_EQ(degenerate.shape.ShapeType(), TopAbs_FACE);
    auto r = steplens::measure::angleBetween(degenerate, edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 10)));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, ZeroHeightPlanarFaceAngleAgainstPlaneIsDegenerateGeometry) {
    const auto degenerate = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 0);
    ASSERT_FALSE(degenerate.shape.IsNull());
    ASSERT_EQ(degenerate.shape.ShapeType(), TopAbs_FACE);
    const auto plane = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(degenerate, plane);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::DegenerateGeometry);
    EXPECT_NE(r.error().userMessage.find("几何退化，无法测量"), std::string::npos);
    EXPECT_EQ(r.error().userMessage.find("不支持该组合"), std::string::npos);
}

TEST(MeasureCompute, SmallNonZeroPlanarFaceAngleRemainsMeasurable) {
    const auto small = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 0.01, 0, 0.01);
    ASSERT_FALSE(small.shape.IsNull());
    const auto plane = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(small, plane);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, PositiveSpanBelowConfusionPlanarFaceAngleRemainsMeasurable) {
    // 正跨度 5e-8 < Precision::Confusion()；探针已记录 MakeFace 成功、adaptor UV 原样非零、BRepCheck 有效。
    const auto thin = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 5e-8, 0, 1);
    ASSERT_FALSE(thin.shape.IsNull());
    ASSERT_EQ(thin.shape.ShapeType(), TopAbs_FACE);
    const BRepAdaptor_Surface surface(TopoDS::Face(thin.shape));
    ASSERT_GT(std::abs(surface.LastUParameter() - surface.FirstUParameter()), 0.0);
    ASSERT_LT(std::abs(surface.LastUParameter() - surface.FirstUParameter()), Precision::Confusion());
    const auto plane = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto r = steplens::measure::angleBetween(thin, plane);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, EquivalentMmAndKmThinFaceAnglesAgree) {
    // 同一 0.05mm×10000mm 矩形：mm 坐标与 km 坐标（U 跨度 5e-8）都应可测，角度与长度单位无关。
    const auto inMm = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 0.05, 0, 10000);
    const auto inKm = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 5e-8, 0, 0.01);
    ASSERT_FALSE(inMm.shape.IsNull());
    ASSERT_FALSE(inKm.shape.IsNull());
    const auto plane = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10);
    auto mm = steplens::measure::angleBetween(inMm, plane);
    auto km = steplens::measure::angleBetween(inKm, plane);
    ASSERT_TRUE(mm.ok());
    ASSERT_TRUE(km.ok());
    EXPECT_NEAR(mm.value().degrees, 0.0, 0.01);
    EXPECT_NEAR(km.value().degrees, 0.0, 0.01);
}

TEST(MeasureCompute, RigidRotationAndTranslationPreserveDistanceAndMoveEndpoints) {
    auto a = vertex(0, 0, 0);
    auto b = vertex(10, 0, 0);
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267948966);
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(3, 4, 5));
    const gp_Trsf rigid = translation * rotation;
    a.transform = rigid;
    b.transform = rigid;
    auto r = steplens::measure::distanceBetween(a, b, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 10.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(3, 4, 5), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(3, 14, 5), 0.001));
}

TEST(MeasureCompute, PointToThinEdgeUnitScaleMatchesAnalyticEndpoints) {
    // P=(0,0,0), E=[(5e-8,1e-9,0),(-1,1e-9,0)], unit=1e6. 解析最短距 0.001mm，落在 Q=(0,1e-9,0)。
    const auto p = vertex(0, 0, 0);
    const auto e = edge(gp_Pnt(5e-8, 1e-9, 0), gp_Pnt(-1, 1e-9, 0));
    constexpr double unit = 1e6;
    auto r = steplens::measure::distanceBetween(p, e, unit);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 0.001, 0.001);
    const double pairMm = r.value().pointA.Distance(r.value().pointB) * unit;
    EXPECT_NEAR(pairMm, 0.001, 0.001);
    EXPECT_NEAR(pairMm, r.value().millimeters, 0.001);
    EXPECT_NEAR(r.value().pointA.Distance(gp_Pnt(0, 0, 0)) * unit, 0.0, 0.001);
    EXPECT_NEAR(r.value().pointB.Distance(gp_Pnt(0, 1e-9, 0)) * unit, 0.0, 0.001);
}

TEST(MeasureCompute, PointToThinEdgeUnitOneRemainsConsistent) {
    const auto p = vertex(0, 0, 0);
    const auto e = edge(gp_Pnt(5e-8, 1e-9, 0), gp_Pnt(-1, 1e-9, 0));
    auto r = steplens::measure::distanceBetween(p, e, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 1e-9, 0.001);
    const double pairMm = r.value().pointA.Distance(r.value().pointB);
    EXPECT_NEAR(pairMm, r.value().millimeters, 0.001);
}

TEST(MeasureCompute, NearCrossingLongSegmentsReportAnalyticZero) {
    // 有限直边在原点相交，解析距 0。缩 deflection 且 Value/端点自洽仍可能错报 0.01。
    const auto a = edge(gp_Pnt(-5e6, 0, 0), gp_Pnt(5e6, 0, 0));
    const auto b = edge(gp_Pnt(-5e6, -0.01, 0), gp_Pnt(5e6, 0.01, 0));
    ASSERT_FALSE(a.shape.IsNull());
    ASSERT_FALSE(b.shape.IsNull());
    ASSERT_FALSE(BRep_Tool::Degenerated(TopoDS::Edge(a.shape)));
    ASSERT_FALSE(BRep_Tool::Degenerated(TopoDS::Edge(b.shape)));
    auto r = steplens::measure::distanceBetween(a, b, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 0.0, 0.001);
    const double pairMm = r.value().pointA.Distance(r.value().pointB);
    EXPECT_NEAR(pairMm, 0.0, 0.001);
    EXPECT_NEAR(pairMm, r.value().millimeters, 0.001);
    EXPECT_NEAR(r.value().pointA.Distance(gp_Pnt(0, 0, 0)), 0.0, 0.001);
    EXPECT_NEAR(r.value().pointB.Distance(gp_Pnt(0, 0, 0)), 0.0, 0.001);
}

TEST(MeasureCompute, NearCrossingLongSegmentsUnitScaleReportAnalyticZero) {
    const auto a = edge(gp_Pnt(-5, 0, 0), gp_Pnt(5, 0, 0));
    const auto b = edge(gp_Pnt(-5, -1e-8, 0), gp_Pnt(5, 1e-8, 0));
    constexpr double unit = 1e6;
    auto r = steplens::measure::distanceBetween(a, b, unit);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 0.0, 0.001);
    const double pairMm = r.value().pointA.Distance(r.value().pointB) * unit;
    EXPECT_NEAR(pairMm, 0.0, 0.001);
    EXPECT_NEAR(pairMm, r.value().millimeters, 0.001);
}

TEST(MeasureCompute, NearCrossingLongSegmentsSwapOrderReportAnalyticZero) {
    const auto a = edge(gp_Pnt(-5e6, 0, 0), gp_Pnt(5e6, 0, 0));
    const auto b = edge(gp_Pnt(-5e6, -0.01, 0), gp_Pnt(5e6, 0.01, 0));
    auto r = steplens::measure::distanceBetween(b, a, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 0.0, 0.001);
    EXPECT_NEAR(r.value().pointA.Distance(r.value().pointB), r.value().millimeters, 0.001);
    EXPECT_NEAR(r.value().pointA.Distance(gp_Pnt(0, 0, 0)), 0.0, 0.001);
    EXPECT_NEAR(r.value().pointB.Distance(gp_Pnt(0, 0, 0)), 0.0, 0.001);
}

TEST(MeasureCompute, ParallelFiniteEdgesKeepPositiveDistance) {
    auto r = steplens::measure::distanceBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
                                                edge(gp_Pnt(0, 5, 0), gp_Pnt(10, 5, 0)), 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 5.0, 0.001);
    EXPECT_NEAR(r.value().pointA.Distance(r.value().pointB), 5.0, 0.001);
}

TEST(MeasureCompute, CrossingFiniteEdgesReportZero) {
    auto r = steplens::measure::distanceBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)),
                                                edge(gp_Pnt(5, -5, 0), gp_Pnt(5, 5, 0)), 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, 0.0, 0.001);
    EXPECT_NEAR(r.value().pointA.Distance(r.value().pointB), 0.0, 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(5, 0, 0), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(5, 0, 0), 0.001));
}

TEST(MeasureCompute, DisjointFiniteEdgeEndsUseClippedEndpoints) {
    auto r = steplens::measure::distanceBetween(edge(gp_Pnt(0, 0, 0), gp_Pnt(1, 0, 0)),
                                                edge(gp_Pnt(2, 1, 0), gp_Pnt(3, 1, 0)), 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().millimeters, std::sqrt(2.0), 0.001);
    EXPECT_TRUE(r.value().pointA.IsEqual(gp_Pnt(1, 0, 0), 0.001));
    EXPECT_TRUE(r.value().pointB.IsEqual(gp_Pnt(2, 1, 0), 0.001));
}

TEST(MeasureCompute, ShortPositiveEdgeKmAndMmExpressionsAgreeRightAngle) {
    const auto shortKm = straightEdge(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), 5e-8);
    const auto refKm = edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 0.01, 0));
    const auto shortMm = edge(gp_Pnt(0, 0, 0), gp_Pnt(0.05, 0, 0));
    const auto refMm = edge(gp_Pnt(0, 0, 0), gp_Pnt(0, 10000, 0));
    ASSERT_FALSE(shortKm.shape.IsNull());
    ASSERT_FALSE(BRep_Tool::Degenerated(TopoDS::Edge(shortKm.shape)));
    ASSERT_GT(BRepAdaptor_Curve(TopoDS::Edge(shortKm.shape)).LastParameter() -
                  BRepAdaptor_Curve(TopoDS::Edge(shortKm.shape)).FirstParameter(),
              0.0);
    auto km = steplens::measure::angleBetween(shortKm, refKm);
    auto mm = steplens::measure::angleBetween(shortMm, refMm);
    ASSERT_TRUE(km.ok());
    ASSERT_TRUE(mm.ok());
    EXPECT_NEAR(km.value().degrees, 90.0, 0.01);
    EXPECT_NEAR(mm.value().degrees, 90.0, 0.01);
}

TEST(MeasureCompute, ShortPositiveEdgeFaceAnglesAgreeInKmAndMm) {
    const auto shortKm = straightEdge(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), 5e-8);
    const auto shortMm = edge(gp_Pnt(0, 0, 0), gp_Pnt(0.05, 0, 0));
    const auto planeKm = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 0.01, 0, 0.01);
    const auto planeMm = planarFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10000, 0, 10000);
    auto efKm = steplens::measure::angleBetween(shortKm, planeKm);
    auto feKm = steplens::measure::angleBetween(planeKm, shortKm);
    auto efMm = steplens::measure::angleBetween(shortMm, planeMm);
    auto feMm = steplens::measure::angleBetween(planeMm, shortMm);
    ASSERT_TRUE(efKm.ok());
    ASSERT_TRUE(feKm.ok());
    ASSERT_TRUE(efMm.ok());
    ASSERT_TRUE(feMm.ok());
    EXPECT_NEAR(efKm.value().degrees, 0.0, 0.01);
    EXPECT_NEAR(feKm.value().degrees, 0.0, 0.01);
    EXPECT_NEAR(efMm.value().degrees, 0.0, 0.01);
    EXPECT_NEAR(feMm.value().degrees, 0.0, 0.01);
}

} // namespace
