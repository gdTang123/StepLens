#include "occtsupport/occt_compat.h"

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_ProgramError.hxx>
#include <StdPrs_ShadedShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#ifndef STEPLENS_EXPECTED_OCCT
#define STEPLENS_EXPECTED_OCCT ""
#endif

// 锁定 StepLens 依赖的 OCCT 行为（复核 A-2 / A-5 / A-9）。
// 7.9.3 与 8.0.1 实测一致，所以这里没有版本分支。行为一旦分叉，断言失败即可；
// 不要把断言改反来凑绿。版本差异只允许写进 occt_compat.h。
// 纯网格面上 BRepMesh_IncrementalMesh::IsDone() 在这两版都是 1。A-9 锁定的是
// BRepExtrema_DistShapeShape::IsDone()==0，不是网格器。

namespace {

constexpr double kCylinderRadius = 10.0;
constexpr double kCylinderHeight = 20.0;
constexpr double kMeshDeflection = 0.01;
constexpr double kMeshAngle = 0.5;

using TriangulationHandle = opencascade::handle<Poly_Triangulation>;
using TriangleArrayHandle = opencascade::handle<Graphic3d_ArrayOfTriangles>;

bool meshShape(const TopoDS_Shape& shape) {
    BRepMesh_IncrementalMesh mesher(shape, kMeshDeflection, false, kMeshAngle, false);
    return static_cast<bool>(mesher.IsDone());
}

TopoDS_Shape makeCylinder() {
    return BRepPrimAPI_MakeCylinder(kCylinderRadius, kCylinderHeight).Shape();
}

} // namespace

TEST(OcctKernelContract, LinkedVersionMatchesBuild) {
    ASSERT_STRNE(STEPLENS_EXPECTED_OCCT, "");
    EXPECT_STREQ(steplens::occt::versionString(), STEPLENS_EXPECTED_OCCT);
}

TEST(OcctKernelContract, SetDoublePrecisionAfterMeshThrows) {
    const TopoDS_Shape shape = makeCylinder();
    ASSERT_TRUE(meshShape(shape));

    TriangulationHandle triangulation;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        TopLoc_Location location;
        triangulation = BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
        if (!triangulation.IsNull()) {
            break;
        }
    }
    ASSERT_FALSE(triangulation.IsNull());
    ASSERT_GT(triangulation->NbNodes(), 0);
    ASSERT_TRUE(triangulation->IsDoublePrecision());

    try {
        triangulation->SetDoublePrecision(false);
        FAIL() << "网格化之后 SetDoublePrecision(false) 没有抛异常";
    } catch (const Standard_ProgramError& failure) {
        EXPECT_NE(std::strstr(steplens::occt::failureMessage(failure), "should be called before allocation"),
                  nullptr);
    }
}

TEST(OcctKernelContract, FillTrianglesWritesNormals) {
    const TopoDS_Shape shape = makeCylinder();
    ASSERT_TRUE(meshShape(shape));

    int faceCount = 0;
    int normalsBefore = 0;
    std::vector<TriangulationHandle> triangulations;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        TopLoc_Location location;
        TriangulationHandle triangulation =
            BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
        ASSERT_FALSE(triangulation.IsNull());
        normalsBefore += triangulation->HasNormals() ? 1 : 0;
        triangulations.push_back(triangulation);
        ++faceCount;
    }
    ASSERT_EQ(faceCount, 3);
    EXPECT_EQ(normalsBefore, 0);

    const TriangleArrayHandle triangles = StdPrs_ShadedShape::FillTriangles(shape);
    ASSERT_FALSE(triangles.IsNull());
    EXPECT_GT(triangles->VertexNumber(), 0);

    int normalsAfter = 0;
    for (const TriangulationHandle& triangulation : triangulations) {
        normalsAfter += triangulation->HasNormals() ? 1 : 0;
    }
    EXPECT_EQ(normalsAfter, faceCount);
}

TEST(OcctKernelContract, IgesReadStreamReturnsRetFail) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(1.0, 2.0, 3.0).Shape();
    IGESControl_Writer writer;
    ASSERT_TRUE(writer.AddShape(box));

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "steplens-occt-contract-box.iges";
    ASSERT_TRUE(writer.Write(path.string().c_str(), false));

    IFSelect_ReturnStatus streamStatus = IFSelect_RetVoid;
    {
        std::ifstream stream(path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        IGESCAFControl_Reader reader;
        streamStatus = reader.ReadStream(path.string().c_str(), stream);
    }
    EXPECT_EQ(streamStatus, IFSelect_RetFail);

    IGESCAFControl_Reader fileReader;
    EXPECT_EQ(fileReader.ReadFile(path.string().c_str()), IFSelect_RetDone);

    std::error_code removed;
    std::filesystem::remove(path, removed);
}

TEST(OcctKernelContract, MeshOnlyFaceDistanceIsNotDone) {
    TriangulationHandle triangulation = new Poly_Triangulation(4, 2, false);
    triangulation->SetNode(1, gp_Pnt(0.0, 0.0, 0.0));
    triangulation->SetNode(2, gp_Pnt(1.0, 0.0, 0.0));
    triangulation->SetNode(3, gp_Pnt(1.0, 1.0, 0.0));
    triangulation->SetNode(4, gp_Pnt(0.0, 1.0, 0.0));
    triangulation->SetTriangle(1, Poly_Triangle(1, 2, 3));
    triangulation->SetTriangle(2, Poly_Triangle(1, 3, 4));

    TopoDS_Face face;
    BRep_Builder builder;
    builder.MakeFace(face, triangulation);
    ASSERT_TRUE(BRep_Tool::Surface(face).IsNull());

    const TopoDS_Shape vertex = BRepBuilderAPI_MakeVertex(gp_Pnt(0.5, 0.5, 1.0)).Shape();
    const BRepExtrema_DistShapeShape distance(vertex, face);
    EXPECT_EQ(static_cast<int>(distance.IsDone()), 0);
}
