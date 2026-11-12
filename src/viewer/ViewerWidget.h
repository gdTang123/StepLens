#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <memory>

class QDragEnterEvent;
class QDropEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

// 默认 0：QOpenGLWidget + Aspect_NeutralWindow + OpenGl_FrameBuffer::InitWrapper。
// 置 1（CMake：-DSTEPLENS_USE_WNT_WINDOW=ON）时改走 WNT_Window。两条路径都在本文件里。
// 团队 Windows 独显机器如果 NeutralWindow 路径画不出非单色画面，切换开关并记在
// docs/bench/t09-gpu-smoke-checklist.md。无 GPU 的 CI 不判像素。
// OCCT 类型只留在 cpp。公开头不传播 TKOpenGl 的包含目录。
#ifndef STEPLENS_USE_WNT_WINDOW
#define STEPLENS_USE_WNT_WINDOW 0
#endif

namespace steplens {

class SceneHost;

// 菜单里的 7 个标准视图。具体 OCCT 朝向留在 cpp。
enum class StandardView : std::uint8_t { Front, Back, Left, Right, Top, Bottom, Isometric };

// 不要把这个控件 reparent 到另一个顶层窗口。上下文销毁时会丢掉 OCCT 资源，
// 下一次 initializeGL 再绑定。清理看 context 是否还在，不看上一帧是否画成功。
// 空闲时仍显示 T09 的旋转方块，已有的独显冒烟脚本靠它判断不是纯色。
// 打开文件之后方块让位给模型。旋转、平移、缩放走 AIS_ViewController，只在 GUI 线程调用。
#if STEPLENS_USE_WNT_WINDOW
class ViewerWidget final : public QWidget {
#else
class ViewerWidget final : public QOpenGLWidget {
#endif
    Q_OBJECT

public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    ~ViewerWidget() override;

    ViewerWidget(const ViewerWidget&) = delete;
    ViewerWidget& operator=(const ViewerWidget&) = delete;

    void fitAll();
    void setStandardView(StandardView view);
    void redraw();
    [[nodiscard]] bool showingModel() const;
    [[nodiscard]] SceneHost& sceneHost();
    [[nodiscard]] QImage grabView();

signals:
    void filesDropped(const QStringList& paths);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
#if STEPLENS_USE_WNT_WINDOW
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
#else
    void initializeGL() override;
    void paintGL() override;
#endif

private:
    friend class ViewerSceneHost;

    void createScene();
    void displayBox();
    void spinBox();
    void releaseScene();
    void suspendIdlePresentation();
    void restoreIdlePresentation();
    void drawScene();
    void noteRedrawRequest(bool again);
    void showTriedron();
#if STEPLENS_USE_WNT_WINDOW
    void bindWntWindow();
#else
    bool wrapQtFramebuffer();
    void bindContextCleanup();
    void releaseGl();
#endif

    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace steplens
