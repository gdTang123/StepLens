#include "app/MainWindow.h"

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
}

} // namespace steplens
