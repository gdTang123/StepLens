#include "viewer/MeasureTextLabel.h"

#include <AIS_TextLabel.hxx>
#include <Graphic3d_AspectText3d.hxx>
#include <Prs3d_TextAspect.hxx>
#include <TCollection_ExtendedString.hxx>
#include <gp_Pnt.hxx>

#include <gtest/gtest.h>

TEST(MeasureTextLabel, ConfigureUsesFixedScreenHeightAndDisablesZoomable) {
    opencascade::handle<AIS_TextLabel> label = new AIS_TextLabel();
    label->SetText(TCollection_ExtendedString("12.345 mm"));
    label->SetPosition(gp_Pnt(1.0, 2.0, 3.0));
    steplens::configureMeasureTextLabel(label);

    ASSERT_FALSE(label.IsNull());
    ASSERT_FALSE(label->Attributes().IsNull());
    ASSERT_FALSE(label->Attributes()->TextAspect().IsNull());
    ASSERT_FALSE(label->Attributes()->TextAspect()->Aspect().IsNull());
    EXPECT_FALSE(label->Attributes()->TextAspect()->Aspect()->GetTextZoomable());
    const double height = label->Attributes()->TextAspect()->Height();
    EXPECT_GE(height, 14.0);
    EXPECT_LE(height, 16.0);
    EXPECT_NEAR(height, steplens::kMeasureLabelPointSize, 1.0e-9);
}
