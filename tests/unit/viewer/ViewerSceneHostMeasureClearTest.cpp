#include "pipeline/SceneHost.h"
#include "ui/MeasureResultModel.h"
#include "viewer/ViewerWidget.h"

#include <QApplication>
#include <QSignalSpy>

#include <gtest/gtest.h>

namespace {

void ensureApplication() {
    static int argc = 1;
    static char name[] = "ViewerSceneHostMeasureClearTest";
    static char* argv[] = {name, nullptr};
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    static QApplication application(argc, argv);
    (void)application;
}

} // namespace

// 钉法（勿真删产品代码来证明）：
// 若删掉 ViewerSceneHost::reset / clear 里 discardDistanceGraphics() 之后的
// emit measurementsCleared，本用例应失败——cleared 信号计数仍为 0，或测量列表仍非空。
// 现有 MeasureResultModel / PropertyPanel 清列表用例不经过宿主，删 emit 仍会通过；本用例必须
// 经 ViewerWidget::sceneHost() 走到产品 ViewerSceneHost。
// clear() 与 reset() 同源 emit；无头下 clear() 末尾 restoreIdlePresentation() 可能 Display/FitAll
// 未绑定窗口，故本用例以 reset(nullptr) 钉换文档路径，并对 clear() 做可失败的附加覆盖。
TEST(ViewerSceneHost, ResetEmitsMeasurementsClearedAndEmptiesList) {
    ensureApplication();

    steplens::ViewerWidget viewer;
    // 不 show()、不打开真实 STEP、不启动 StepLens.exe。
    steplens::ui::MeasureResultModel list;
    list.addDistanceMillimeters(12.345);
    list.addAngleDegrees(45.0);
    ASSERT_EQ(list.count(), 2);

    int cleared = 0;
    QObject::connect(&viewer, &steplens::ViewerWidget::measurementsCleared, &viewer, [&] {
        ++cleared;
        list.clearResults();
    });
    QSignalSpy spy(&viewer, &steplens::ViewerWidget::measurementsCleared);

    steplens::SceneHost& host = viewer.sceneHost();
    host.reset(nullptr);

    EXPECT_GE(cleared, 1) << "reset() 必须 emit measurementsCleared（换文档路径）";
    EXPECT_FALSE(spy.isEmpty()) << "QSignalSpy 应收到 measurementsCleared";
    EXPECT_EQ(list.count(), 0) << "列表应随 measurementsCleared 被清空";

    // 附加：clear() 同源 emit。若无头 Display/FitAll 抛异常则跳过，不影响 reset 钉法。
    list.addDistanceMillimeters(1.0);
    list.addAngleDegrees(2.0);
    ASSERT_EQ(list.count(), 2);
    const int clearedBeforeClear = cleared;
    const int spyBeforeClear = spy.count();
    try {
        host.clear();
        EXPECT_GT(cleared, clearedBeforeClear) << "clear() 必须 emit measurementsCleared";
        EXPECT_GT(spy.count(), spyBeforeClear);
        EXPECT_EQ(list.count(), 0);
    } catch (const std::exception& ex) {
        GTEST_SKIP() << "无头 clear()/restoreIdlePresentation 不可用: " << ex.what()
                     << "；reset 钉法已覆盖同源 emit";
    } catch (...) {
        GTEST_SKIP() << "无头 clear()/restoreIdlePresentation 不可用；reset 钉法已覆盖同源 emit";
    }
}
