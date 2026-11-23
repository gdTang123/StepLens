#pragma once

#include "app/StatusNotice.h"
#include "core/LifecycleTrace.h"
#include "model/DocumentModel.h"

#include <QMainWindow>
#include <QString>

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
namespace ui {
class AssemblyTreePanel;
class PropertyPanel;
}

// 主窗口。打开、取消、进度都走 LoadPipeline，界面线程只 tick，不读文件、不三角化。
// 真实成员/子树拆除顺序由 STEPLENS_LIFECYCLE_LOG 记录；自定义析构体结束不等于整窗已拆完。
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
    void showSelectedNode(model::NodeId id);
    void showPartMenu(bool hasNode, model::NodeId id, QPoint globalPos);
    void showOnlyCurrent();
    void showAllParts();
    void exportScreenshot();
    void saveOverlayShot();
    void saveModelShots();
    void endProcessNow();

    struct WindowLifetimeTrace {
        const char* event;
        explicit WindowLifetimeTrace(const char* event) noexcept : event(event) {}
        ~WindowLifetimeTrace() {
            lifecycleTrace(event);
        }
    };

    class NullSceneHost;
    // 先声明的成员后析构：members-end 标在其余成员拆除之后、QObject 子树之前。
    WindowLifetimeTrace m_traceMembersEnd{"mainwindow-members-end"};
    std::unique_ptr<NullSceneHost> m_nullHost;
    std::unique_ptr<LoadPipeline> m_pipeline;
    QWidget* m_frame = nullptr;
    ViewerWidget* m_viewer = nullptr;
    LoadingOverlay* m_overlay = nullptr;
    ui::AssemblyTreePanel* m_treePanel = nullptr;
    ui::PropertyPanel* m_propertyPanel = nullptr;
    std::shared_ptr<const model::DocumentModel> m_treeDocument;
    QLabel* m_hint = nullptr;
    QLabel* m_selectionLabel = nullptr;
    QLabel* m_unitLabel = nullptr;
    QLabel* m_fileLabel = nullptr;
    bool m_hasSelection = false;
    model::NodeId m_selectedId = 0;
    QString m_dumpDir;
    StatusNotice m_memoryNotice;
    bool m_recentFileRecorded = false;
    bool m_overlayShotSaved = false;
    bool m_modelShotSaved = false;
    bool m_dumpQuitArmed = false;
    bool m_quitWhenReadyArmed = false;
    bool m_closeWhileBusyArmed = false;
    // 后声明的成员先析构：自定义析构体结束后立刻记下成员拆除开始。
    WindowLifetimeTrace m_traceMembersBegin{"mainwindow-members-begin"};
};

} // namespace steplens
