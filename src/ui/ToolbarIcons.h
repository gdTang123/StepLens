#pragma once

#include <QIcon>
#include <QString>

namespace steplens::ui {

// 程序绘制的单色线框图标，不引入外部资源或第三方图标包。
enum class ToolbarGlyph : quint8 {
    Open,
    Fit,
    ViewFront,
    ViewBack,
    ViewLeft,
    ViewRight,
    ViewTop,
    ViewBottom,
    ViewIso,
    ShadeWithEdges,
    ShadeOnly,
    Wireframe,
    Distance,
    Angle,
    ClearMeasure,
    Screenshot,
};

[[nodiscard]] QIcon toolbarIcon(ToolbarGlyph glyph, int size = 20);

} // namespace steplens::ui
