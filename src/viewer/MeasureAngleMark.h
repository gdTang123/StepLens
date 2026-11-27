#pragma once

// 角度三维标注：可见锚点、遮挡层、弧线。数值仍由 measure::angleBetween 计算。

#include "measure/DistanceMeasure.h"

#include <AIS_InteractiveObject.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

namespace steplens {

struct AngleMarkPlacement {
    gp_Pnt hinge{};
    gp_Pnt labelAnchor{};
    TopoDS_Shape arc;
    bool valid = false;
};

// 两形状交线（或等价特征）外侧锚点 + 可见弧。失败时 valid=false。
[[nodiscard]] AngleMarkPlacement computeAngleMarkPlacement(const measure::MeasureShape& a,
                                                           const measure::MeasureShape& b);

// 测距/测角 overlay：世界坐标保留，置顶层避免被模型深度挡住。
void configureMeasureOverlayObject(const opencascade::handle<AIS_InteractiveObject>& object);

} // namespace steplens
