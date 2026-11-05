#pragma once

#include <QOpenGLWidget>
#include <QWidget>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;

// 默认 0：QOpenGLWidget + Aspect_NeutralWindow + OpenGl_FrameBuffer::InitWrapper。
// 置 1（CMake：-DSTEPLENS_USE_WNT_WINDOW=ON）时改走 WNT_Window。两条路径都在本文件里。
// 团队 Windows 独显机器如果 NeutralWindow 路径画不出非单色画面，切换开关并记在
// docs/bench/t09-gpu-smoke-checklist.md。无 GPU 的 CI 不判像素。
#ifndef STEPLENS_USE_WNT_WINDOW
#define STEPLENS_USE_WNT_WINDOW 0
#endif

namespace steplens {

#if STEPLENS_USE_WNT_WINDOW
class ViewerWidget final : public QWidget {
#else
class ViewerWidget final : public QOpenGLWidget {
#endif
    Q_OBJECT

public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    ~ViewerWidget() override;

protected:
#if STEPLENS_USE_WNT_WINDOW
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
#else
    void initializeGL() override;
    void paintGL() override;
#endif

private:
    void createScene();
    void displayBox();
    void spinBox();
    void releaseScene();
#if STEPLENS_USE_WNT_WINDOW
    void bindWntWindow();
#else
    bool wrapQtFramebuffer();
#endif

    QTimer* m_timer = nullptr;
    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;
    Handle(AIS_Shape) m_box;
    double m_angle = 0.0;
    bool m_glReady = false;
    bool m_fboWarned = false;
};

} // namespace steplens
