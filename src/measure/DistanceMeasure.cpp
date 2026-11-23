#include "measure/DistanceMeasure.h"

#include "occtsupport/OcctExceptions.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Precision.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <gp_Vec.hxx>

#include <cmath>

namespace {

bool finitePoint(const gp_Pnt& point) {
    return std::isfinite(point.X()) && std::isfinite(point.Y()) && std::isfinite(point.Z());
}

gp_Pnt pointOnSegment(const gp_Pnt& origin, const gp_Vec& vec, double t) {
    return gp_Pnt(origin.X() + t * vec.X(), origin.Y() + t * vec.Y(), origin.Z() + t * vec.Z());
}

void considerPair(const gp_Pnt& a, const gp_Pnt& b, gp_Pnt& bestA, gp_Pnt& bestB, double& best, bool& have) {
    if (!finitePoint(a) || !finitePoint(b))
        return;
    const double distance = a.Distance(b);
    if (!std::isfinite(distance) || distance < 0.0)
        return;
    if (!have || distance < best) {
        best = distance;
        bestA = a;
        bestB = b;
        have = true;
    }
}

gp_Pnt closestOnSegment(const gp_Pnt& query, const gp_Pnt& origin, const gp_Vec& vec, double vecDotVec) {
    if (!(vecDotVec > 0.0) || !std::isfinite(vecDotVec))
        return origin;
    const gp_Vec offset(origin, query);
    double t = offset.Dot(vec) / vecDotVec;
    if (t < 0.0)
        t = 0.0;
    else if (t > 1.0)
        t = 1.0;
    return pointOnSegment(origin, vec, t);
}

// Finite straight-segment closest points in IEEE double only.
// Frame is origin a0 with axis U/|U|. The perpendicular part of V is recovered by
// subtracting the parallel component, so a small transverse offset on a long segment
// is not lost to 1-dot^2 cancellation. Candidates are the interior minimum of the
// transverse residual on the parameter overlap, plus clipped endpoint projections.
bool closestFiniteLineSegments(const gp_Pnt& a0, const gp_Pnt& a1, const gp_Pnt& b0, const gp_Pnt& b1,
                               gp_Pnt& outA, gp_Pnt& outB, double& outDistance) {
    const gp_Vec u(a0, a1);
    const gp_Vec v(b0, b1);
    const double lenU = u.Magnitude();
    const double lenV = v.Magnitude();
    if (!std::isfinite(lenU) || !std::isfinite(lenV) || lenU == 0.0 || lenV == 0.0)
        return false;
    const gp_Vec uHat(u.X() / lenU, u.Y() / lenU, u.Z() / lenU);
    if (!std::isfinite(uHat.X()) || !std::isfinite(uHat.Y()) || !std::isfinite(uHat.Z()))
        return false;

    const gp_Vec w(a0, b0);
    const double wPar = w.Dot(uHat);
    const gp_Vec wPerp(w.X() - wPar * uHat.X(), w.Y() - wPar * uHat.Y(), w.Z() - wPar * uHat.Z());
    const double vPar = v.Dot(uHat);
    const gp_Vec vPerp(v.X() - vPar * uHat.X(), v.Y() - vPar * uHat.Y(), v.Z() - vPar * uHat.Z());
    if (!std::isfinite(wPar) || !std::isfinite(vPar))
        return false;

    gp_Pnt bestA, bestB;
    double best = 0.0;
    bool have = false;

    const auto pointOnA = [&](double x) {
        double clipped = x;
        if (clipped < 0.0)
            clipped = 0.0;
        else if (clipped > lenU)
            clipped = lenU;
        return gp_Pnt(a0.X() + clipped * uHat.X(), a0.Y() + clipped * uHat.Y(), a0.Z() + clipped * uHat.Z());
    };
    const auto considerT = [&](double t) {
        if (t < 0.0)
            t = 0.0;
        else if (t > 1.0)
            t = 1.0;
        considerPair(pointOnA(wPar + t * vPar), pointOnSegment(b0, v, t), bestA, bestB, best, have);
    };

    double tLo = 0.0;
    double tHi = 1.0;
    bool overlap = true;
    if (vPar == 0.0) {
        overlap = (wPar >= 0.0 && wPar <= lenU);
    } else {
        double tAt0 = (0.0 - wPar) / vPar;
        double tAtLen = (lenU - wPar) / vPar;
        if (tAt0 > tAtLen) {
            const double tmp = tAt0;
            tAt0 = tAtLen;
            tAtLen = tmp;
        }
        tLo = tAt0 > 0.0 ? tAt0 : 0.0;
        tHi = tAtLen < 1.0 ? tAtLen : 1.0;
        overlap = std::isfinite(tLo) && std::isfinite(tHi) && tLo <= tHi;
    }
    if (overlap) {
        const double vPerp2 = vPerp.Dot(vPerp);
        if (std::isfinite(vPerp2) && vPerp2 > 0.0) {
            const double tStar = -wPerp.Dot(vPerp) / vPerp2;
            if (std::isfinite(tStar) && tStar >= tLo && tStar <= tHi)
                considerT(tStar);
        }
        considerT(tLo);
        considerT(tHi);
    }

    const double vDotV = v.Dot(v);
    considerPair(a0, closestOnSegment(a0, b0, v, vDotV), bestA, bestB, best, have);
    considerPair(a1, closestOnSegment(a1, b0, v, vDotV), bestA, bestB, best, have);
    considerT(0.0);
    considerT(1.0);
    if (!have)
        return false;
    outA = bestA;
    outB = bestB;
    outDistance = best;
    return true;
}

} // namespace

namespace steplens::measure {
Result<DistanceValue> distanceBetween(const MeasureShape& a, const MeasureShape& b, double lengthUnitToMm) {
    if (!std::isfinite(lengthUnitToMm) || lengthUnitToMm <= 0.0)
        return Error{ErrorCode::Internal, "invalid length unit", "长度单位无效"};
    if (a.shape.IsNull() || b.shape.IsNull())
        return Error{ErrorCode::DegenerateGeometry, "null measurement shape", "几何退化，无法测量"};

    return occt::catchOcctAsResult(
        [&]() -> Result<DistanceValue> {
            const TopoDS_Shape worldA = a.shape.Moved(TopLoc_Location(a.transform), Standard_True);
            const TopoDS_Shape worldB = b.shape.Moved(TopLoc_Location(b.transform), Standard_True);

            const auto isDegeneratedEdge = [](const TopoDS_Shape& shape) {
                return shape.ShapeType() == TopAbs_EDGE && BRep_Tool::Degenerated(TopoDS::Edge(shape));
            };
            if (isDegeneratedEdge(worldA) || isDegeneratedEdge(worldB))
                return Error{ErrorCode::DegenerateGeometry, "degenerated edge", "几何退化，无法测量"};

            if (worldA.ShapeType() == TopAbs_EDGE && worldB.ShapeType() == TopAbs_EDGE) {
                const BRepAdaptor_Curve curveA(TopoDS::Edge(worldA));
                const BRepAdaptor_Curve curveB(TopoDS::Edge(worldB));
                if (curveA.GetType() == GeomAbs_Line && curveB.GetType() == GeomAbs_Line) {
                    const gp_Pnt startA = curveA.Value(curveA.FirstParameter());
                    const gp_Pnt endA = curveA.Value(curveA.LastParameter());
                    const gp_Pnt startB = curveB.Value(curveB.FirstParameter());
                    const gp_Pnt endB = curveB.Value(curveB.LastParameter());
                    gp_Pnt pointA;
                    gp_Pnt pointB;
                    double model = 0.0;
                    if (!closestFiniteLineSegments(startA, endA, startB, endB, pointA, pointB, model))
                        return Error{ErrorCode::DegenerateGeometry, "line segment closest failed",
                                     "几何退化，无法测量"};
                    const double millimeters = model * lengthUnitToMm;
                    if (!std::isfinite(millimeters) || millimeters < 0.0 || !finitePoint(pointA) ||
                        !finitePoint(pointB))
                        return Error{ErrorCode::DegenerateGeometry, "non-finite distance result",
                                     "几何退化，无法测量"};
                    return DistanceValue{millimeters, pointA, pointB};
                }
            }

            if (worldA.ShapeType() == TopAbs_FACE && worldB.ShapeType() == TopAbs_FACE) {
                const BRepAdaptor_Surface surfaceA(TopoDS::Face(worldA));
                const BRepAdaptor_Surface surfaceB(TopoDS::Face(worldB));
                if (surfaceA.GetType() != GeomAbs_Plane || surfaceB.GetType() != GeomAbs_Plane ||
                    !surfaceA.Plane().Axis().Direction().IsParallel(surfaceB.Plane().Axis().Direction(),
                                                                    Precision::Angular())) {
                    return Error{ErrorCode::UnsupportedCombination, "unsupported face combination",
                                 "不支持该组合"};
                }
            }

            // Deflection is the extrema solver tolerance in model units, not a proof of
            // millimetre accuracy. Tighten it when the 0.001mm budget is stricter than
            // Confusion; never loosen the default, never pass a non-positive value, and
            // do not SetDeflection after an auto-Perform constructor.
            constexpr double kBudgetMm = 0.001;
            const double budgetModel = kBudgetMm / lengthUnitToMm;
            double deflection = Precision::Confusion();
            if (std::isfinite(budgetModel) && budgetModel > 0.0 && budgetModel < deflection)
                deflection = budgetModel;
            if (!std::isfinite(deflection) || deflection <= 0.0)
                return Error{ErrorCode::DegenerateGeometry, "invalid solver tolerance", "几何退化，无法测量"};

            BRepExtrema_DistShapeShape extrema(worldA, worldB, deflection);
            if (!extrema.IsDone() || extrema.NbSolution() <= 0) {
                return Error{ErrorCode::DegenerateGeometry, "distance extrema failed", "几何退化，无法测量"};
            }

            const auto finitePoint = [](const gp_Pnt& point) {
                return std::isfinite(point.X()) && std::isfinite(point.Y()) && std::isfinite(point.Z());
            };
            int best = 1;
            double bestModel = extrema.PointOnShape1(1).Distance(extrema.PointOnShape2(1));
            for (Standard_Integer i = 2; i <= extrema.NbSolution(); ++i) {
                const double candidate = extrema.PointOnShape1(i).Distance(extrema.PointOnShape2(i));
                if (std::isfinite(candidate) && candidate < bestModel) {
                    bestModel = candidate;
                    best = i;
                }
            }
            const gp_Pnt pointA = extrema.PointOnShape1(best);
            const gp_Pnt pointB = extrema.PointOnShape2(best);
            const double millimetersFromValue = extrema.Value() * lengthUnitToMm;
            const double millimetersFromPoints = bestModel * lengthUnitToMm;
            if (!std::isfinite(millimetersFromValue) || millimetersFromValue < 0.0 ||
                !std::isfinite(millimetersFromPoints) || millimetersFromPoints < 0.0 ||
                !finitePoint(pointA) || !finitePoint(pointB) ||
                std::abs(millimetersFromValue - millimetersFromPoints) > kBudgetMm) {
                return Error{ErrorCode::DegenerateGeometry, "distance endpoints miss budget",
                             "几何退化，无法测量"};
            }
            return DistanceValue{millimetersFromPoints, pointA, pointB};
        },
        ErrorCode::DegenerateGeometry, "几何退化，无法测量");
}
} // namespace steplens::measure
