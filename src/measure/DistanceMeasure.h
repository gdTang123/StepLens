#pragma once

#include "core/Result.h"

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

namespace steplens::measure {

struct MeasureShape {
    // 原型/本地形状，可自带 TopoDS Location；不得剥掉该 Location。
    TopoDS_Shape shape;
    // 附加的实例到世界变换（类似 AssemblyNode::worldLocation），只附加一次。
    // 世界形状为 shape.Moved(TopLoc_Location(transform))；调用方不得将同一实例变换
    // 同时烤进 shape 并放入 transform。
    gp_Trsf transform;
};

struct DistanceValue {
    double millimeters;
    gp_Pnt pointA;
    gp_Pnt pointB;
};

// 先在世界模型单位计算距离，再仅将距离乘 lengthUnitToMm 一次；端点保持世界模型坐标。
// 空/Null shape、退化几何應返回 DegenerateGeometry（提示「几何退化，无法测量」）；
// 非有限或非正的 lengthUnitToMm 应返回错误（提示「长度单位无效」）。
Result<DistanceValue> distanceBetween(const MeasureShape& a, const MeasureShape& b, double lengthUnitToMm);

} // namespace steplens::measure
