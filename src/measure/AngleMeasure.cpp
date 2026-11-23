#include "measure/AngleMeasure.h"

#include "occtsupport/OcctExceptions.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace steplens::measure {
Result<AngleValue> angleBetween(const MeasureShape& a, const MeasureShape& b) {
    constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
    constexpr std::string_view kDegenerateMessage = "几何退化，无法测量";
    constexpr std::string_view kUnsupportedMessage = "不支持该组合";

    return occt::catchOcctAsResult(
        [&]() -> Result<AngleValue> {
            if (a.shape.IsNull() || b.shape.IsNull())
                return Error{ErrorCode::DegenerateGeometry, "null measurement shape",
                             std::string{kDegenerateMessage}};

            const TopoDS_Shape worldA = a.shape.Moved(TopLoc_Location(a.transform), Standard_True);
            const TopoDS_Shape worldB = b.shape.Moved(TopLoc_Location(b.transform), Standard_True);

            const auto isDegeneratedEdge = [](const TopoDS_Shape& shape) {
                return shape.ShapeType() == TopAbs_EDGE && BRep_Tool::Degenerated(TopoDS::Edge(shape));
            };
            if (isDegeneratedEdge(worldA) || isDegeneratedEdge(worldB))
                return Error{ErrorCode::DegenerateGeometry, "degenerated edge",
                             std::string{kDegenerateMessage}};

            const auto edgeDirection = [&](const TopoDS_Shape& shape) -> Result<gp_Dir> {
                BRepAdaptor_Curve curve(TopoDS::Edge(shape));
                if (curve.GetType() != GeomAbs_Line)
                    return Error{ErrorCode::UnsupportedCombination, "unsupported edge curve",
                                 std::string{kUnsupportedMessage}};

                const double first = curve.FirstParameter();
                const double last = curve.LastParameter();
                if (!std::isfinite(first) || !std::isfinite(last) || first == last)
                    return Error{ErrorCode::DegenerateGeometry, "zero-length edge",
                                 std::string{kDegenerateMessage}};

                const gp_Pnt firstPoint = curve.Value(first);
                const gp_Pnt lastPoint = curve.Value(last);
                if (!std::isfinite(firstPoint.X()) || !std::isfinite(firstPoint.Y()) ||
                    !std::isfinite(firstPoint.Z()) || !std::isfinite(lastPoint.X()) ||
                    !std::isfinite(lastPoint.Y()) || !std::isfinite(lastPoint.Z()))
                    return Error{ErrorCode::DegenerateGeometry, "zero-length edge",
                                 std::string{kDegenerateMessage}};

                return curve.Line().Direction();
            };

            const auto faceNormal = [&](const TopoDS_Shape& shape) -> Result<gp_Dir> {
                BRepAdaptor_Surface surface(TopoDS::Face(shape));
                if (surface.GetType() != GeomAbs_Plane)
                    return Error{ErrorCode::UnsupportedCombination, "unsupported face surface",
                                 std::string{kUnsupportedMessage}};

                // 只拒绝 NaN 或有限参数上精确零跨度；不把正跨度与 Confusion 比较，也不扫描子边。
                const double uFirst = surface.FirstUParameter();
                const double uLast = surface.LastUParameter();
                const double vFirst = surface.FirstVParameter();
                const double vLast = surface.LastVParameter();
                if (std::isnan(uFirst) || std::isnan(uLast) || std::isnan(vFirst) || std::isnan(vLast))
                    return Error{ErrorCode::DegenerateGeometry, "degenerate planar face",
                                 std::string{kDegenerateMessage}};
                const bool uZeroSpan = std::isfinite(uFirst) && std::isfinite(uLast) && uFirst == uLast;
                const bool vZeroSpan = std::isfinite(vFirst) && std::isfinite(vLast) && vFirst == vLast;
                if (uZeroSpan || vZeroSpan)
                    return Error{ErrorCode::DegenerateGeometry, "degenerate planar face",
                                 std::string{kDegenerateMessage}};
                return surface.Plane().Axis().Direction();
            };

            const bool edgeA = worldA.ShapeType() == TopAbs_EDGE;
            const bool edgeB = worldB.ShapeType() == TopAbs_EDGE;
            const bool faceA = worldA.ShapeType() == TopAbs_FACE;
            const bool faceB = worldB.ShapeType() == TopAbs_FACE;
            if ((!edgeA && !faceA) || (!edgeB && !faceB) || (edgeA && !edgeB && !faceB) ||
                (faceA && !edgeB && !faceB))
                return Error{ErrorCode::UnsupportedCombination, "unsupported shape combination",
                             std::string{kUnsupportedMessage}};

            gp_Dir directionA(1.0, 0.0, 0.0);
            gp_Dir directionB(1.0, 0.0, 0.0);
            if (edgeA) {
                auto result = edgeDirection(worldA);
                if (!result.ok())
                    return result.error();
                directionA = result.value();
            } else {
                auto result = faceNormal(worldA);
                if (!result.ok())
                    return result.error();
                directionA = result.value();
            }
            if (edgeB) {
                auto result = edgeDirection(worldB);
                if (!result.ok())
                    return result.error();
                directionB = result.value();
            } else {
                auto result = faceNormal(worldB);
                if (!result.ok())
                    return result.error();
                directionB = result.value();
            }

            double cosine = directionA.Dot(directionB);
            if (!std::isfinite(cosine))
                return Error{ErrorCode::DegenerateGeometry, "non-finite angle input",
                             std::string{kDegenerateMessage}};
            cosine = std::clamp(std::abs(cosine), 0.0, 1.0);

            double radians = 0.0;
            if (edgeA != edgeB) {
                radians = std::asin(cosine);
            } else {
                radians = std::acos(cosine);
            }
            const double degrees = radians * kRadiansToDegrees;
            if (!std::isfinite(degrees))
                return Error{ErrorCode::DegenerateGeometry, "non-finite angle result",
                             std::string{kDegenerateMessage}};
            return AngleValue{degrees};
        },
        ErrorCode::DegenerateGeometry, kDegenerateMessage);
}
} // namespace steplens::measure
