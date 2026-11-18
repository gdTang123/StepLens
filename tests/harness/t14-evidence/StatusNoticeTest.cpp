#include "app/StatusNotice.h"

#include <QApplication>
#include <QMainWindow>
#include <QStatusBar>
#include <QTimer>

#include <gtest/gtest.h>

TEST(StatusNotice, CombinedNoticeRemainsVisibleAcrossUiTicks) {
    // This Widgets harness drives the exact StatusNotice type used by MainWindow.
    int argc = 1;
    char name[] = "t14_evidence";
    char* argv[] = {name, nullptr};
    QApplication app(argc, argv);
    QMainWindow window;
    steplens::StatusNotice notice;
    notice.setMemoryConditions(true, true);
    QTimer tick;
    tick.setInterval(16);
    int ticks = 0;
    QObject::connect(&tick, &QTimer::timeout, [&] {
        ++ticks;
        if (ticks == 1) {
            window.statusBar()->showMessage(notice.combine(QStringLiteral("正在读取文件")));
            notice.finish();
        } else {
            window.statusBar()->showMessage(notice.combine(QStringLiteral("已完成")));
        }
        if (ticks == 5) {
            EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("配置无效")));
            EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("预检不可用")));
            EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("已完成")));
            tick.stop();
            app.quit();
        }
    });
    tick.start();
    QTimer::singleShot(200, &app, &QApplication::quit);
    app.exec();
    EXPECT_EQ(ticks, 5);
}
