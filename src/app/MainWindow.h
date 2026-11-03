#pragma once

#include <QMainWindow>

namespace steplens {

// T01 的空窗口。装配树、三维视图和菜单在 T09–T11 再接上。
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
};

} // namespace steplens
