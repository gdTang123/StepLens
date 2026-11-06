#include "viewer/ViewerWidget.h"

#include "occtsupport/occt_compat.h"

#include <QDebug>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSurfaceFormat>
#include <QTimer>

#include <AIS_DisplayMode.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#if STEPLENS_USE_WNT_WINDOW
#include <WNT_Window.hxx>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>

#if defined(_WIN32) && !STEPLENS_USE_WNT_WINDOW
// Qt 的头在包含 windows.h 时会定义 NOGDI 和 NOUSER，wingdi.h / winuser.h 里的函数被整段跳过。
// 包含守卫让后面再包含 windows.h 不会展开，所以按 SDK 原型补上 NeutralWindow 要用的两个函数。
extern "C" {
__declspec(dllimport) HDC __stdcall wglGetCurrentDC(void);
__declspec(dllimport) HWND __stdcall WindowFromDC(HDC);
}
#endif

namespace steplens {
namespace {

constexpr int kFrameIntervalMs = 16;
constexpr double kFullTurn = 6.283185307179586;

} // namespace

ViewerWidget::ViewerWidget(QWidget* parent)
#if STEPLENS_USE_WNT_WINDOW
    : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
#else
    : QOpenGLWidget(parent) {
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(format);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
#endif
    setMinimumSize(320, 240);
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] { update(); });
    try {
        createScene();
    } catch (const Standard_Failure& failure) {
        qWarning("StepLens: viewer scene init failed: %s", steplens::occt::failureMessage(failure));
    } catch (const std::exception& ex) {
        qWarning("StepLens: viewer scene init failed: %s", ex.what());
    } catch (...) {
        qWarning("StepLens: viewer scene init failed");
    }
}

ViewerWidget::~ViewerWidget() {
    if (m_timer != nullptr) {
        m_timer->stop();
    }
#if !STEPLENS_USE_WNT_WINDOW
    if (m_glReady) {
        makeCurrent();
    }
#endif
    releaseScene();
#if !STEPLENS_USE_WNT_WINDOW
    if (m_glReady) {
        doneCurrent();
    }
#endif
}

void ViewerWidget::createScene() {
    Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
    // false：不要在构造时自建 GL 上下文，交给 Qt（或 WNT_Window）已经当前的那个。
    Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(display, false);
#if STEPLENS_USE_WNT_WINDOW
    driver->ChangeOptions().buffersNoSwap = false;
#else
    // Qt 负责交换缓冲。OCCT 画进 QOpenGLWidget 的 FBO，不写 alpha。
    driver->ChangeOptions().buffersNoSwap = true;
    driver->ChangeOptions().buffersOpaqueAlpha = true;
    driver->ChangeOptions().useSystemBuffer = false;
#endif
    m_viewer = new V3d_Viewer(driver);
    m_viewer->SetDefaultLights();
    m_viewer->SetLightOn();
    m_view = m_viewer->CreateView();
    m_view->SetImmediateUpdate(false);
    m_view->SetBackgroundColor(Quantity_TOC_RGB, 0.16, 0.18, 0.22);
    m_context = new AIS_InteractiveContext(m_viewer);
    m_box = new AIS_Shape(BRepPrimAPI_MakeBox(100.0, 50.0, 90.0).Shape());
}

void ViewerWidget::displayBox() {
    m_context->Display(m_box, AIS_Shaded, -1, false);
    m_view->FitAll(0.01, false);
    m_glReady = true;
    m_timer->start(kFrameIntervalMs);
}

void ViewerWidget::spinBox() {
    m_angle += 0.03;
    if (m_angle > kFullTurn) {
        m_angle -= kFullTurn;
    }
    gp_Trsf trsf;
    trsf.SetRotation(gp_Ax1(gp_Pnt(50.0, 25.0, 45.0), gp_Dir(0.0, 0.0, 1.0)), m_angle);
    m_box->SetLocalTransformation(trsf);
    m_context->Redisplay(m_box, false);
}

void ViewerWidget::releaseScene() {
    m_box.Nullify();
    m_context.Nullify();
    m_view.Nullify();
    m_viewer.Nullify();
    m_glReady = false;
}

#if STEPLENS_USE_WNT_WINDOW

void ViewerWidget::bindWntWindow() {
    if (m_view.IsNull() || m_glReady) {
        return;
    }
    try {
        auto* nativePtr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(winId()));
        const auto handle = static_cast<Aspect_Handle>(nativePtr);
        Handle(WNT_Window) window = new WNT_Window(handle);
        m_view->SetWindow(window);
        if (!window->IsMapped()) {
            window->Map();
        }
        m_view->MustBeResized();
        displayBox();
    } catch (const Standard_Failure& failure) {
        qWarning("StepLens: WNT_Window bind failed: %s", steplens::occt::failureMessage(failure));
    } catch (const std::exception& ex) {
        qWarning("StepLens: WNT_Window bind failed: %s", ex.what());
    } catch (...) {
        qWarning("StepLens: WNT_Window bind failed");
    }
}

void ViewerWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    bindWntWindow();
}

void ViewerWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_glReady && !m_view.IsNull()) {
        m_view->MustBeResized();
        m_view->FitAll(0.01, false);
    }
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    if (!m_glReady || m_view.IsNull()) {
        return;
    }
    try {
        spinBox();
        m_view->Redraw();
    } catch (const Standard_Failure& failure) {
        qWarning("StepLens: WNT redraw failed: %s", steplens::occt::failureMessage(failure));
        m_glReady = false;
        m_timer->stop();
    } catch (...) {
        m_glReady = false;
        m_timer->stop();
    }
}

#else

void ViewerWidget::initializeGL() {
    if (m_view.IsNull()) {
        return;
    }
    try {
        Handle(OpenGl_Context) gl = new OpenGl_Context();
        if (!gl->Init(true)) {
            qWarning("StepLens: OpenGl_Context::Init failed; 3D smoke stays blank");
            return;
        }
        const int pixelWidth = std::max(1, static_cast<int>(std::lround(width() * devicePixelRatioF())));
        const int pixelHeight = std::max(1, static_cast<int>(std::lround(height() * devicePixelRatioF())));
        Handle(Aspect_NeutralWindow) window = new Aspect_NeutralWindow();
        auto* nativePtr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(winId()));
        Aspect_Drawable native = static_cast<Aspect_Drawable>(nativePtr);
#if defined(_WIN32)
        if (const HDC dc = wglGetCurrentDC()) {
            if (const HWND hwnd = WindowFromDC(dc)) {
                native = static_cast<Aspect_Drawable>(hwnd);
            }
        }
#endif
        window->SetNativeHandle(native);
        window->SetSize(pixelWidth, pixelHeight);
        m_view->SetWindow(window, gl->RenderingContext());
        displayBox();
    } catch (const Standard_Failure& failure) {
        qWarning("StepLens: NeutralWindow init failed: %s", steplens::occt::failureMessage(failure));
    } catch (const std::exception& ex) {
        qWarning("StepLens: NeutralWindow init failed: %s", ex.what());
    } catch (...) {
        qWarning("StepLens: NeutralWindow init failed");
    }
}

bool ViewerWidget::wrapQtFramebuffer() {
    Handle(OpenGl_GraphicDriver) driver =
        Handle(OpenGl_GraphicDriver)::DownCast(m_context->CurrentViewer()->Driver());
    if (driver.IsNull()) {
        return false;
    }
    const Handle(OpenGl_Context) & gl = driver->GetSharedContext();
    if (gl.IsNull()) {
        return false;
    }
    Handle(OpenGl_FrameBuffer) framebuffer = gl->DefaultFrameBuffer();
    if (framebuffer.IsNull()) {
        framebuffer = new OpenGl_FrameBuffer();
        gl->SetDefaultFrameBuffer(framebuffer);
    }
    if (!framebuffer->InitWrapper(gl)) {
        if (!m_fboWarned) {
            m_fboWarned = true;
            // 无 GPU / 离屏时这里会失败。进程继续跑，像素检查留给清单，不在 CI 里判。
            qWarning("StepLens: OpenGl_FrameBuffer::InitWrapper failed");
        }
        return false;
    }
    Handle(Aspect_NeutralWindow) window = Handle(Aspect_NeutralWindow)::DownCast(m_view->Window());
    if (window.IsNull()) {
        return false;
    }
    const int pixelWidth = std::max(framebuffer->GetVPSizeX(), 1);
    const int pixelHeight = std::max(framebuffer->GetVPSizeY(), 1);
    int oldWidth = 0;
    int oldHeight = 0;
    window->Size(oldWidth, oldHeight);
    if (oldWidth != pixelWidth || oldHeight != pixelHeight) {
        window->SetSize(pixelWidth, pixelHeight);
        m_view->MustBeResized();
        m_view->FitAll(0.01, false);
        m_view->Invalidate();
    }
    return true;
}

void ViewerWidget::paintGL() {
    if (!m_glReady || m_view.IsNull() || m_context.IsNull()) {
        return;
    }
    try {
        if (!wrapQtFramebuffer()) {
            return;
        }
        spinBox();
        m_view->Redraw();
    } catch (const Standard_Failure& failure) {
        qWarning("StepLens: paintGL failed: %s", steplens::occt::failureMessage(failure));
        m_glReady = false;
        m_timer->stop();
    } catch (...) {
        m_glReady = false;
        m_timer->stop();
    }
}

#endif

} // namespace steplens
