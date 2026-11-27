#include "viewer/MeasureAngleMark.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
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

constexpr double kParallelDotEps = 1.0e-7;
constexpr double kCrossEps2 = 1.0e-14;

bool finitePnt(const gp_Pnt& p) {
    return std::isfinite(p.X()) && std::isfinite(p.Y()) && std::isfinite(p.Z());
}

gp_Dir stablePerpendicularTo(const gp_Dir& axis) {
    const gp_Vec ref = (std::abs(axis.X()) < 0.9) ? gp_Vec(1.0, 0.0, 0.0) : gp_Vec(0.0, 1.0, 0.0);
    return gp_Dir(ref.Crossed(gp_Vec(axis)));
}

// 单位方向之和为外角平分线；近乎相反时无稳定垂直方向，不留不可达的叉积分支。
gp_Dir outwardBisector(const gp_Dir& a, const gp_Dir& b) {
    const gp_Vec sum(a.XYZ() + b.XYZ());
    if (sum.SquareMagnitude() < 1.0e-12) {
        return stablePerpendicularTo(a);
    }
    return gp_Dir(sum);
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

// 平面几何方向 + Face Orientation → 真实外法向。
bool tryFaceOutwardNormal(const TopoDS_Shape& world, gp_Pln& plane, gp_Dir& outwardNormal) {
    if (!tryPlane(world, plane)) {
        return false;
    }
    outwardNormal = plane.Axis().Direction();
    if (TopoDS::Face(world).Orientation() == TopAbs_REVERSED) {
        outwardNormal.Reverse();
    }
    return true;
}

bool tryLine(const TopoDS_Shape& world, gp_Lin& line, gp_Pnt& mid) {
    if (world.IsNull() || world.ShapeType() != TopAbs_EDGE) {
        return false;
    }
    const TopoDS_Edge edge = TopoDS::Edge(world);
    if (BRep_Tool::Degenerated(edge)) {
        return false;
    }
    BRepAdaptor_Curve curve(edge);
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

bool faceHinge(const TopoDS_Shape& faceA, const TopoDS_Shape& faceB, gp_Pnt& hinge, gp_Dir& outwardA,
               gp_Dir& outwardB) {
    gp_Pln planeA;
    gp_Pln planeB;
    if (!tryFaceOutwardNormal(faceA, planeA, outwardA) ||
        !tryFaceOutwardNormal(faceB, planeB, outwardB)) {
        return false;
    }

    for (TopExp_Explorer edgesA(faceA, TopAbs_EDGE); edgesA.More(); edgesA.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(edgesA.Current());
        if (BRep_Tool::Degenerated(edge)) {
            continue;
        }
        for (TopExp_Explorer edgesB(faceB, TopAbs_EDGE); edgesB.More(); edgesB.Next()) {
            if (!edge.IsSame(TopoDS::Edge(edgesB.Current()))) {
                continue;
            }
            if (BRep_Tool::Degenerated(TopoDS::Edge(edgesB.Current()))) {
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

// 平行（夹角约 0°）不画弧，返回空 shape。
TopoDS_Shape makeArc(const gp_Pnt& hinge, const gp_Dir& fromDir, const gp_Dir& toDir, double radius) {
    if (!(radius > 0.0) || !std::isfinite(radius)) {
        return {};
    }
    const gp_Vec cross = gp_Vec(fromDir).Crossed(gp_Vec(toDir));
    if (cross.SquareMagnitude() < kCrossEps2) {
        return {};
    }
    const gp_Dir planeNormal(cross);
    const gp_Pnt pFrom = hinge.Translated(gp_Vec(fromDir) * radius);
    const gp_Pnt pTo = hinge.Translated(gp_Vec(toDir) * radius);
    const gp_Dir midDir = outwardBisector(fromDir, toDir);
    const gp_Pnt pMid = hinge.Translated(gp_Vec(midDir) * radius);
    GC_MakeArcOfCircle maker(pFrom, pMid, pTo);
    if (!maker.IsDone()) {
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

// 两真实外法向 → 面内腿，张角等于锐角 θ（angleBetween 契约），且朝向外侧。
bool faceFaceArcLegs(const gp_Dir& outwardA, const gp_Dir& outwardB, const gp_Dir& outward,
                     gp_Dir& arcFrom, gp_Dir& arcTo) {
    gp_Dir nA = outwardA;
    gp_Dir nB = outwardB;
    // 仅构弧时对齐为锐角，不改用于文字外偏的真实外法向。
    if (nA.Dot(nB) < 0.0) {
        nB.Reverse();
    }
    gp_Vec edgeApprox = gp_Vec(nA).Crossed(gp_Vec(nB));
    if (edgeApprox.SquareMagnitude() < kCrossEps2) {
        return false;
    }
    const gp_Dir edgeDir(edgeApprox);
    // 同序叉积：inA·inB = cosθ，张角为 θ，而不是 π-θ。
    gp_Vec inA = gp_Vec(edgeDir).Crossed(gp_Vec(nA));
    gp_Vec inB = gp_Vec(edgeDir).Crossed(gp_Vec(nB));
    if (inA.SquareMagnitude() < kCrossEps2 || inB.SquareMagnitude() < kCrossEps2) {
        return false;
    }
    arcFrom = gp_Dir(inA);
    arcTo = gp_Dir(inB);
    // 一对翻转保持张角 θ，使角平分线与真实外侧同向。
    if (outwardBisector(arcFrom, arcTo).Dot(outward) < 0.0) {
        arcFrom.Reverse();
        arcTo.Reverse();
    }
    return true;
}

bool edgeFaceArcLegs(const gp_Dir& edgeDir, const gp_Dir& outwardNormal, gp_Dir& arcFrom,
                     gp_Dir& arcTo) {
    gp_Dir d = edgeDir;
    const double dn = d.Dot(outwardNormal);
    // θ≈0°：边平行于面，不画弧。
    if (std::abs(dn) <= kParallelDotEps) {
        return false;
    }
    if (dn < 0.0) {
        d.Reverse();
    }
    const gp_Vec dParaVec = gp_Vec(d) - gp_Vec(outwardNormal) * d.Dot(outwardNormal);
    const double paraMag2 = dParaVec.SquareMagnitude();
    if (paraMag2 <= kCrossEps2) {
        // θ=90°：边垂直于面，取稳定面内基向量。
        arcFrom = d;
        arcTo = stablePerpendicularTo(outwardNormal);
        if (outwardBisector(arcFrom, arcTo).Dot(outwardNormal) < 0.0) {
            arcTo.Reverse();
        }
        return true;
    }
    gp_Dir dPara(dParaVec);
    if (d.Dot(dPara) < 0.0) {
        dPara.Reverse();
    }
    arcFrom = d;
    arcTo = dPara;
    return true;
}

} // namespace

AngleMarkPlacement computeAngleMarkPlacement(const measure::MeasureShape& a,
                                             const measure::MeasureShape& b) {
    AngleMarkPlacement out;
    if (a.shape.IsNull() || b.shape.IsNull()) {
        return out;
    }
    try {
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
        enum class PairKind { FaceFace, EdgeEdge, EdgeFace, Unknown };
        PairKind kind = PairKind::Unknown;

        if (worldA.ShapeType() == TopAbs_FACE && worldB.ShapeType() == TopAbs_FACE) {
            ok = faceHinge(worldA, worldB, hinge, dirA, dirB);
            kind = PairKind::FaceFace;
        } else if (worldA.ShapeType() == TopAbs_EDGE && worldB.ShapeType() == TopAbs_EDGE) {
            gp_Lin lineA;
            gp_Lin lineB;
            gp_Pnt midA;
            gp_Pnt midB;
            if (tryLine(worldA, lineA, midA) && tryLine(worldB, lineB, midB)) {
                dirA = lineA.Direction();
                dirB = lineB.Direction();
                if (dirA.Dot(dirB) < 0.0) {
                    dirB.Reverse();
                }
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
                kind = PairKind::EdgeEdge;
            }
        } else if ((worldA.ShapeType() == TopAbs_EDGE && worldB.ShapeType() == TopAbs_FACE) ||
                   (worldA.ShapeType() == TopAbs_FACE && worldB.ShapeType() == TopAbs_EDGE)) {
            const TopoDS_Shape& edgeShape = worldA.ShapeType() == TopAbs_EDGE ? worldA : worldB;
            const TopoDS_Shape& faceShape = worldA.ShapeType() == TopAbs_FACE ? worldA : worldB;
            gp_Lin line;
            gp_Pnt mid;
            gp_Pln plane;
            gp_Dir outwardNormal;
            if (tryLine(edgeShape, line, mid) && tryFaceOutwardNormal(faceShape, plane, outwardNormal)) {
                BRepExtrema_DistShapeShape dist(edgeShape, faceShape);
                if (dist.IsDone() && dist.NbSolution() >= 1) {
                    hinge = dist.PointOnShape1(1);
                } else {
                    hinge = mid;
                }
                dirA = line.Direction();
                dirB = outwardNormal;
                ok = finitePnt(hinge);
                kind = PairKind::EdgeFace;
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

        gp_Dir outward = outwardBisector(dirA, dirB);
        if (kind == PairKind::EdgeFace) {
            // 文字外偏：沿面外法向（dirB）一侧。
            outward = dirB;
        }
        out.hinge = hinge;
        out.labelAnchor = hinge.Translated(gp_Vec(outward) * offset);
        if (!finitePnt(out.labelAnchor)) {
            return out;
        }

        const double radius = std::max(0.5, offset * 0.65);
        gp_Dir arcFrom = dirA;
        gp_Dir arcTo = dirB;
        bool drawArc = false;
        if (kind == PairKind::FaceFace) {
            drawArc = faceFaceArcLegs(dirA, dirB, outward, arcFrom, arcTo);
        } else if (kind == PairKind::EdgeFace) {
            drawArc = edgeFaceArcLegs(dirA, dirB, arcFrom, arcTo);
        } else {
            // EdgeEdge / Unknown：dirB 已按锐角对齐；近平行则不画弧。
            drawArc = gp_Vec(dirA).Crossed(gp_Vec(dirB)).SquareMagnitude() >= kCrossEps2;
        }

        if (drawArc) {
            out.arc = makeArc(hinge, arcFrom, arcTo, radius);
        }
        out.valid = true;
        return out;
    } catch (const Standard_Failure&) {
        return AngleMarkPlacement{};
    }
}

void configureMeasureOverlayObject(const opencascade::handle<AIS_InteractiveObject>& object) {
    if (object.IsNull()) {
        return;
    }
    // Topmost：三维 overlay、独立深度，避免被模型挡住；仍用世界坐标锚点（非屏幕乱飘）。
    object->SetZLayer(Graphic3d_ZLayerId_Topmost);
}

} // namespace steplens
