#pragma once

#include <QMainWindow>

#include <filesystem>
#include <memory>

class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QLabel;
class QObject;

namespace steplens {

class LoadPipeline;
class LoadingOverlay;
class SceneHost;
class ViewerWidget;

// 主窗口。打开、取消、进度都走 LoadPipeline，界面线程只 tick，不读文件、不三角化。
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openPath(const QString& file);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildShell();
    void buildMenus();
    void refreshUi();
    void layoutView();
    void openFiles(const QStringList& files);
    void showFailure(const QString& message);
    void saveOverlayShot();
    void saveModelShots();
    void endProcessNow();

    class NullSceneHost;
    std::unique_ptr<NullSceneHost> m_nullHost;
    std::unique_ptr<LoadPipeline> m_pipeline;
    QWidget* m_frame = nullptr;
    ViewerWidget* m_viewer = nullptr;
    LoadingOverlay* m_overlay = nullptr;
    QLabel* m_hint = nullptr;
    QLabel* m_fileLabel = nullptr;
    QString m_dumpDir;
    bool m_overlayShotSaved = false;
    bool m_modelShotSaved = false;
    bool m_dumpQuitArmed = false;
};

} // namespace steplens
