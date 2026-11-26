#include "ui/MeasureResultModel.h"
#include "ui/PropertyPanel.h"

#include <QApplication>

#include <gtest/gtest.h>

namespace {

void ensureApplication() {
    static int argc = 1;
    static char name[] = "MeasureResultModelTest";
    static char* argv[] = {name, nullptr};
    static QApplication application(argc, argv);
    (void)application;
}

} // namespace

TEST(MeasureResultModel, DistanceAngleClearWithoutGeometry) {
    ensureApplication();
    steplens::ui::MeasureResultModel model;

    model.addDistanceMillimeters(12.3456);
    model.addDistanceMillimeters(1.0);
    model.addAngleDegrees(90.0);
    model.addAngleDegrees(12.345);
    ASSERT_EQ(model.count(), 4);
    EXPECT_EQ(model.at(0).text, QStringLiteral("12.346 mm"));
    EXPECT_EQ(model.at(1).text, QStringLiteral("1.000 mm"));
    EXPECT_EQ(model.at(2).text, QStringLiteral("90.00°"));
    EXPECT_EQ(model.at(3).text, QStringLiteral("12.35°"));
    EXPECT_EQ(model.data(model.index(0), Qt::DisplayRole).toString(), QStringLiteral("12.346 mm"));

    model.clearResults();
    EXPECT_EQ(model.count(), 0);
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(MeasureResultModel, UnitScaleUsesUserDisplayFactor) {
    ensureApplication();
    EXPECT_EQ(steplens::ui::MeasureResultModel::formatDistance(25.4, 1.0 / 25.4, QStringLiteral("in")),
              QStringLiteral("1.000 in"));
    EXPECT_EQ(steplens::ui::MeasureResultModel::formatAngle(45.0), QStringLiteral("45.00°"));
}

TEST(PropertyPanel, ClearMeasurementsAndDisplayUnitWithoutGeometry) {
    ensureApplication();
    steplens::ui::PropertyPanel panel;
    panel.setDisplayLengthUnit(1.0 / 25.4, QStringLiteral("in"));
    panel.addDistanceResult(25.4);
    panel.addDistanceResult(50.8);
    panel.addAngleResult(90.0);
    panel.addAngleResult(12.345);
    ASSERT_EQ(panel.measureModel()->count(), 4);
    EXPECT_EQ(panel.measureModel()->at(0).text, QStringLiteral("1.000 in"));
    EXPECT_EQ(panel.measureModel()->at(1).text, QStringLiteral("2.000 in"));
    EXPECT_EQ(panel.measureModel()->at(2).text, QStringLiteral("90.00°"));
    EXPECT_EQ(panel.measureModel()->at(3).text, QStringLiteral("12.35°"));

    panel.clearMeasurements();
    EXPECT_EQ(panel.measureModel()->count(), 0);

    panel.setDisplayLengthUnit(1.0, QStringLiteral("mm"));
    panel.addDistanceResult(3.14159);
    ASSERT_EQ(panel.measureModel()->count(), 1);
    EXPECT_EQ(panel.measureModel()->at(0).text, QStringLiteral("3.142 mm"));
}
