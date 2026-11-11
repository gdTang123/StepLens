#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QDragEnterEvent;
class QDropEvent;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QResizeEvent;

namespace steplens {

// 盖在三维视图上，不挡住菜单。布局用 Qt 的设备无关尺寸，125% / 150% 跟系统缩放走。
class LoadingOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit LoadingOverlay(QWidget* parent = nullptr);

    void setStatus(const QString& title, const QString& detail, float fraction, bool indeterminate,
                   bool cancelEnabled);

signals:
    void cancelRequested();
    void filesDropped(const QStringList& paths);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void placeCard();
    QFrame* m_card = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_detail = nullptr;
    QProgressBar* m_bar = nullptr;
    QPushButton* m_cancel = nullptr;
};

} // namespace steplens
