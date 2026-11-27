#include "viewer/ViewerWidget.h"

#include <QApplication>
#include <QSignalSpy>
#include <QString>

#include <gtest/gtest.h>

namespace {

void ensureApplication() {
    static int argc = 1;
    static char name[] = "ViewerMeasurePromptTest";
    static char* argv[] = {name, nullptr};
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    static QApplication application(argc, argv);
    (void)application;
}

QString lastMessage(const QSignalSpy& spy) {
    if (spy.isEmpty()) {
        return {};
    }
    return spy.last().at(0).toString();
}

} // namespace

TEST(ViewerMeasurePrompt, BeginAndCancelDistanceEmitsStageMessages) {
    ensureApplication();
    steplens::ViewerWidget viewer;
    QSignalSpy spy(&viewer, &steplens::ViewerWidget::measureMessage);

    viewer.beginPointDistance();
    ASSERT_FALSE(spy.isEmpty());
    const QString enter = lastMessage(spy);
    EXPECT_FALSE(enter.isEmpty());
    EXPECT_TRUE(enter.contains(QStringLiteral("距离"))) << enter.toStdString();
    EXPECT_TRUE(enter.contains(QStringLiteral("第一"))) << enter.toStdString();
    EXPECT_TRUE(viewer.pointDistanceActive());

    viewer.cancelPointDistance();
    const QString cancel = lastMessage(spy);
    EXPECT_FALSE(cancel.isEmpty());
    EXPECT_NE(cancel, enter);
    EXPECT_TRUE(cancel.contains(QStringLiteral("取消"))) << cancel.toStdString();
    EXPECT_FALSE(viewer.pointDistanceActive());
}

TEST(ViewerMeasurePrompt, BeginAndCancelAngleEmitsStageMessages) {
    ensureApplication();
    steplens::ViewerWidget viewer;
    QSignalSpy spy(&viewer, &steplens::ViewerWidget::measureMessage);

    viewer.beginAngle();
    ASSERT_FALSE(spy.isEmpty());
    const QString enter = lastMessage(spy);
    EXPECT_FALSE(enter.isEmpty());
    EXPECT_TRUE(enter.contains(QStringLiteral("角度"))) << enter.toStdString();
    EXPECT_TRUE(enter.contains(QStringLiteral("第一"))) << enter.toStdString();
    EXPECT_TRUE(viewer.angleActive());

    viewer.cancelAngle();
    const QString cancel = lastMessage(spy);
    EXPECT_FALSE(cancel.isEmpty());
    EXPECT_NE(cancel, enter);
    EXPECT_TRUE(cancel.contains(QStringLiteral("取消"))) << cancel.toStdString();
    EXPECT_FALSE(viewer.angleActive());
}
