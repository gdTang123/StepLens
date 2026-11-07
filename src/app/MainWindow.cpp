#include "app/MainWindow.h"

#include "viewer/ViewerWidget.h"

#include <QLabel>
#include <QStatusBar>

namespace steplens {

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("StepLens"));
    resize(kDefaultWidth, kDefaultHeight);
    statusBar()->showMessage(tr("尚未打开文件"));
    // QT_QPA_PLATFORM=offscreen 的 CI 没有可判定的 GPU 帧缓冲。
    // 像素、DPI、resize 见 docs/bench/t09-gpu-smoke-checklist.md，不在这里失败。
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") {
        auto* placeholder = new QLabel(tr("离屏模式，未创建三维视图"), this);
        placeholder->setAlignment(Qt::AlignCenter);
        setCentralWidget(placeholder);
        return;
    }
    setCentralWidget(new ViewerWidget(this));
}

} // namespace steplens
