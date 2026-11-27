#include "viewer/MeasureAngleMark.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

namespace steplens {
namespace {

bool finitePnt(const gp_Pnt& p) {
    return std::isfinite(p.X()) && std::isfinite(p.Y()) && std::isfinite(p.Z());
}

bool tryPlane(const TopoDS_Shape& world, gp_Pln& plane) {
    if (world.IsNull() || world.ShapeType() != TopAbs_FACE) {
        return false;
    }
    BRepAdaptor_Surface surface(TopoDS::Face(world));
    if (surface.GetType() != GeomAbs_Plane) {
        return false;
    }
    plane = surface.Plane();
    return true;
}

bool tryLine(const TopoDS_Shape& world, gp_Lin& line, gp_Pnt& mid) {
    if (world.IsNull() || world.ShapeType() != TopAbs_EDGE) {
        return false;
    }
    BRepAdaptor_Curve curve(TopoDS::Edge(world));
    if (curve.GetType() != GeomAbs_Line) {
        return false;
    }
    const double first = curve.FirstParameter();
    const double last = curve.LastParameter();
    if (!std::isfinite(first) || !std::isfinite(last) || first == last) {
        return false;
    }
    line = curve.Line();
    mid = curve.Value(0.5 * (first + last));
    return finitePnt(mid);
}

double shapeSizeHint(const TopoDS_Shape& a, const TopoDS_Shape& b) {
    Bnd_Box box;
    BRepBndLib::Add(a, box);
    BRepBndLib::Add(b, box);
    if (box.IsVoid()) {
        return 5.0;
    }
    const double diagonal = box.CornerMin().Distance(box.CornerMax());
    if (!std::isfinite(diagonal) || diagonal <= 0.0) {
        return 5.0;
    }
    return std::max(1.0, diagonal * 0.08);
}

gp_Dir outwardBisector(const gp_Dir& a, const gp_Dir& b) {
    gp_Vec sum(a.XYZ() + b.XYZ());
    if (sum.SquareMagnitude() < 1.0e-12) {
        gp_Vec cross = gp_Vec(a).Crossed(gp_Vec(b));
        if (cross.SquareMagnitude() < 1.0e-12) {
            return a;
        }
        return gp_Dir(cross);
    }
    return gp_Dir(sum);
}

bool faceHinge(const TopoDS_Shape& faceA, const TopoDS_Shape& faceB, gp_Pnt& hinge, gp_Dir& normalA,
               gp_Dir& normalB) {
    gp_Pln planeA;
    gp_Pln planeB;
    if (!tryPlane(faceA, planeA) || !tryPlane(faceB, planeB)) {
        return false;
    }
    normalA = planeA.Axis().Direction();
    normalB = planeB.Axis().Direction();

    // 优先：落在两面边界上的共享边中点。
    for (TopExp_Explorer edgesA(faceA, TopAbs_EDGE); edgesA.More(); edgesA.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(edgesA.Current());
        for (TopExp_Explorer edgesB(faceB, TopAbs_EDGE); edgesB.More(); edgesB.Next()) {
            if (!edge.IsSame(TopoDS::Edge(edgesB.Current()))) {
                continue;
            }
            BRepAdaptor_Curve curve(edge);
            const double first = curve.FirstParameter();
            const double last = curve.LastParameter();
            if (!std::isfinite(first) || !std::isfinite(last) || first == last) {
                continue;
            }
            hinge = curve.Value(0.5 * (first + last));
            if (finitePnt(hinge)) {
                return true;
            }
        }
    }

    // 退路：两面最近点中点（正交长方体相邻面也会落到交线附近）。
    BRepExtrema_DistShapeShape dist(faceA, faceB);
    if (!dist.IsDone() || dist.NbSolution() < 1) {
        return false;
    }
    const gp_Pnt pA = dist.PointOnShape1(1);
    const gp_Pnt pB = dist.PointOnShape2(1);
    if (!finitePnt(pA) || !finitePnt(pB)) {
        return false;
    }
    hinge = gp_Pnt(0.5 * (pA.X() + pB.X()), 0.5 * (pA.Y() + pB.Y()), 0.5 * (pA.Z() + pB.Z()));
    return finitePnt(hinge);
}

TopoDS_Shape makeArc(const gp_Pnt& hinge, const gp_Dir& fromDir, const gp_Dir& toDir, double radius) {
    if (!(radius > 0.0) || !std::isfinite(radius)) {
        return {};
    }
    gp_Vec cross = gp_Vec(fromDir).Crossed(gp_Vec(toDir));
    if (cross.SquareMagnitude() < 1.0e-14) {
        // 平行：画一小段折线代替弧。
        const gp_Pnt a = hinge.Translated(gp_Vec(fromDir) * radius);
        const gp_Pnt b = hinge.Translated(gp_Vec(toDir) * radius);
        BRepBuilderAPI_MakeEdge edge(a, b);
        return edge.IsDone() ? edge.Shape() : TopoDS_Shape{};
    }
    const gp_Dir planeNormal(cross);
    const gp_Pnt pFrom = hinge.Translated(gp_Vec(fromDir) * radius);
    const gp_Pnt pTo = hinge.Translated(gp_Vec(toDir) * radius);
    const gp_Dir midDir = outwardBisector(fromDir, toDir);
    const gp_Pnt pMid = hinge.Translated(gp_Vec(midDir) * radius);
    GC_MakeArcOfCircle maker(pFrom, pMid, pTo);
    if (!maker.IsDone()) {
        // 三点弧失败时用圆参数弧。
        gp_Ax2 axes(hinge, planeNormal, fromDir);
        gp_Circ circ(axes, radius);
        const double angle = gp_Vec(fromDir).Angle(gp_Vec(toDir));
        if (!std::isfinite(angle) || angle <= 0.0) {
            return {};
        }
        BRepBuilderAPI_MakeEdge edge(circ, 0.0, angle);
        return edge.IsDone() ? edge.Shape() : TopoDS_Shape{};
    }
    BRepBuilderAPI_MakeEdge edge(maker.Value());
    return edge.IsDone() ? edge.Shape() : TopoDS_Shape{};
}

} // namespace

AngleMarkPlacement computeAngleMarkPlacement(const measure::MeasureShape& a,
                                             const measure::MeasureShape& b) {
    AngleMarkPlacement out;
    if (a.shape.IsNull() || b.shape.IsNull()) {
        return out;
    }
    const TopoDS_Shape worldA = a.shape.Moved(TopLoc_Location(a.transform), true);
    const TopoDS_Shape worldB = b.shape.Moved(TopLoc_Location(b.transform), true);
    if (worldA.IsNull() || worldB.IsNull()) {
        return out;
    }

    const double offset = shapeSizeHint(worldA, worldB);
    gp_Pnt hinge;
    gp_Dir dirA(1.0, 0.0, 0.0);
    gp_Dir dirB(0.0, 1.0, 0.0);
    bool ok = false;

    if (worldA.ShapeType() == TopAbs_FACE && worldB.ShapeType() == TopAbs_FACE) {
        ok = faceHinge(worldA, worldB, hinge, dirA, dirB);
    } else if (worldA.ShapeType() == TopAbs_EDGE && worldB.ShapeType() == TopAbs_EDGE) {
        gp_Lin lineA;
        gp_Lin lineB;
        gp_Pnt midA;
        gp_Pnt midB;
        if (tryLine(worldA, lineA, midA) && tryLine(worldB, lineB, midB)) {
            dirA = lineA.Direction();
            dirB = lineB.Direction();
            BRepExtrema_DistShapeShape dist(worldA, worldB);
            if (dist.IsDone() && dist.NbSolution() >= 1) {
                const gp_Pnt pA = dist.PointOnShape1(1);
                const gp_Pnt pB = dist.PointOnShape2(1);
                hinge = gp_Pnt(0.5 * (pA.X() + pB.X()), 0.5 * (pA.Y() + pB.Y()),
                               0.5 * (pA.Z() + pB.Z()));
                ok = finitePnt(hinge);
            } else {
                hinge = gp_Pnt(0.5 * (midA.X() + midB.X()), 0.5 * (midA.Y() + midB.Y()),
                               0.5 * (midA.Z() + midB.Z()));
                ok = finitePnt(hinge);
            }
        }
    } else {
        // 边-面：铰点取最近点，方向用边切向与面法向的正交分解。
        const TopoDS_Shape& edgeShape = worldA.ShapeType() == TopAbs_EDGE ? worldA : worldB;
        const TopoDS_Shape& faceShape = worldA.ShapeType() == TopAbs_FACE ? worldA : worldB;
        gp_Lin line;
        gp_Pnt mid;
        gp_Pln plane;
        if (tryLine(edgeShape, line, mid) && tryPlane(faceShape, plane)) {
            BRepExtrema_DistShapeShape dist(edgeShape, faceShape);
            if (dist.IsDone() && dist.NbSolution() >= 1) {
                hinge = dist.PointOnShape1(1);
            } else {
                hinge = mid;
            }
            dirA = line.Direction();
            dirB = plane.Axis().Direction();
            ok = finitePnt(hinge);
        }
    }

    if (!ok) {
        BRepExtrema_DistShapeShape dist(worldA, worldB);
        if (dist.IsDone() && dist.NbSolution() >= 1) {
            const gp_Pnt pA = dist.PointOnShape1(1);
            const gp_Pnt pB = dist.PointOnShape2(1);
            hinge = gp_Pnt(0.5 * (pA.X() + pB.X()), 0.5 * (pA.Y() + pB.Y()),
                           0.5 * (pA.Z() + pB.Z()));
            ok = finitePnt(hinge);
        }
    }
    if (!ok) {
        return out;
    }

    const gp_Dir outward = outwardBisector(dirA, dirB);
    out.hinge = hinge;
    out.labelAnchor = hinge.Translated(gp_Vec(outward) * offset);
    if (!finitePnt(out.labelAnchor)) {
        return out;
    }

    // 弧落在两方向张成的平面上，半径略小于文字偏移，便于看到。
    const double radius = std::max(0.5, offset * 0.65);
    // 面法向角：弧端点沿面内、垂直于交线的方向更直观。
    gp_Dir arcFrom = dirA;
    gp_Dir arcTo = dirB;
    if (worldA.ShapeType() == TopAbs_FACE && worldB.ShapeType() == TopAbs_FACE) {
        gp_Vec edgeApprox = gp_Vec(dirA).Crossed(gp_Vec(dirB));
        if (edgeApprox.SquareMagnitude() > 1.0e-14) {
            const gp_Dir edgeDir(edgeApprox);
            gp_Vec inA = gp_Vec(edgeDir).Crossed(gp_Vec(dirA));
            gp_Vec inB = gp_Vec(dirB).Crossed(gp_Vec(edgeDir));
            if (inA.SquareMagnitude() > 1.0e-14 && inB.SquareMagnitude() > 1.0e-14) {
                // 取朝向外侧（与 outward 同侧）的面内方向。
                if (inA.Dot(gp_Vec(outward)) < 0.0) {
                    inA.Reverse();
                }
                if (inB.Dot(gp_Vec(outward)) < 0.0) {
                    inB.Reverse();
                }
                arcFrom = gp_Dir(inA);
                arcTo = gp_Dir(inB);
            }
        }
    }
    out.arc = makeArc(hinge, arcFrom, arcTo, radius);
    out.valid = true;
    return out;
}

void configureMeasureOverlayObject(const opencascade::handle<AIS_InteractiveObject>& object) {
    if (object.IsNull()) {
        return;
    }
    // Topmost：三维 overlay、独立深度，避免被模型挡住；仍用世界坐标锚点（非屏幕乱飘）。
    object->SetZLayer(Graphic3d_ZLayerId_Topmost);
}

} // namespace steplens
