#include "viewer/MeasureTextLabel.h"

#include <Quantity_Color.hxx>

#include <Aspect_TypeOfDisplayText.hxx>

namespace steplens {

void configureMeasureTextLabel(const opencascade::handle<AIS_TextLabel>& label) {
    if (label.IsNull()) {
        return;
    }
    // 关闭世界单位随视图放大，避免小零件上字高接近半个特征并被裁糊。
    label->SetZoomable(false);
    label->SetHeight(kMeasureLabelPointSize);
    label->SetDisplayType(Aspect_TODT_SUBTITLE);
    // 浅色衬底抬可读性，尽量少遮挡。
    label->SetColorSubTitle(Quantity_Color(0.96, 0.96, 0.94, Quantity_TOC_RGB));
}

} // namespace steplens
