#pragma once

// 测距/测角 AIS_TextLabel 共用配置：屏幕固定字号，不随场景缩放。

#include <AIS_TextLabel.hxx>

namespace steplens {

// 非 Zoomable 时 Height 作为 Font PointSize。取 14–16 中间值。
inline constexpr Standard_Real kMeasureLabelPointSize = 15.0;

void configureMeasureTextLabel(const opencascade::handle<AIS_TextLabel>& label);

} // namespace steplens
