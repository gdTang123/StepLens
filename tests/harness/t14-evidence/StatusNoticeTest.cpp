#include "app/StatusNotice.h"

#include <QApplication>
#include <QMainWindow>
#include <QStatusBar>
#include <QTest>

#include <gtest/gtest.h>

TEST(StatusNotice, CombinedNoticeRemainsVisibleAcrossUiTicks) {
    // This Widgets harness drives the exact StatusNotice type used by MainWindow.
    int argc = 1;
    char name[] = "t14_evidence";
    char* argv[] = {name, nullptr};
    QApplication app(argc, argv);
    QMainWindow window;
    window.show();
    steplens::StatusNotice notice;
    notice.setMemoryConditions(true, true);
    window.statusBar()->showMessage(notice.combine(QStringLiteral("正在读取文件")));
    notice.finish();

    // Refresh across multiple UI timer periods without depending on timer delivery.
    for (int tick = 0; tick < 5; ++tick) {
        QTest::qWait(16);
        window.statusBar()->showMessage(notice.combine(QStringLiteral("已完成")));
    }

    const QString message = window.statusBar()->currentMessage();
    EXPECT_TRUE(message.contains(QStringLiteral("配置无效")));
    EXPECT_TRUE(message.contains(QStringLiteral("预检不可用")));
    EXPECT_TRUE(message.contains(QStringLiteral("已完成")));
}
