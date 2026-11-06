#include "viewer/ViewerWidget.h"

#include "occtsupport/occt_compat.h"
#include "viewer/GlCleanup.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSurfaceFormat>
#include <QTimer>

#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Standard_Failure.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
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

struct ViewerWidget::State {
    QTimer* timer = nullptr;
    Handle(V3d_Viewer) viewer;
    Handle(V3d_View) view;
    Handle(AIS_InteractiveContext) context;
    Handle(AIS_Shape) box;
    double angle = 0.0;
    bool paintReady = false;
    bool fboWarned = false;
    bool resourcesAlive = false;
    bool cleanupConnected = false;
};

ViewerWidget::ViewerWidget(QWidget* parent)
#if STEPLENS_USE_WNT_WINDOW
    : QWidget(parent),
#else
    : QOpenGLWidget(parent),
#endif
      m_state(std::make_unique<State>()) {
#if STEPLENS_USE_WNT_WINDOW
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
#else
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(format);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
#endif
    setMinimumSize(320, 240);
    m_state->timer = new QTimer(this);
    connect(m_state->timer, &QTimer::timeout, this, [this] { update(); });
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
    if (m_state->timer != nullptr) {
        m_state->timer->stop();
    }
#if STEPLENS_USE_WNT_WINDOW
    releaseScene();
#else
    // 基类析构才会销毁 context。先断开，避免 aboutToBeDestroyed 在 m_state 析构之后再进来。
    if (QOpenGLContext* ctx = context()) {
        ctx->disconnect(this);
    }
    releaseGl();
#endif
}

void ViewerWidget::createScene() {
    m_state->resourcesAlive = true;
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
    m_state->viewer = new V3d_Viewer(driver);
    m_state->viewer->SetDefaultLights();
    m_state->viewer->SetLightOn();
    m_state->view = m_state->viewer->CreateView();
    m_state->view->SetImmediateUpdate(false);
    m_state->view->SetBackgroundColor(Quantity_TOC_RGB, 0.16, 0.18, 0.22);
    m_state->context = new AIS_InteractiveContext(m_state->viewer);
    m_state->box = new AIS_Shape(BRepPrimAPI_MakeBox(100.0, 50.0, 90.0).Shape());
}

void ViewerWidget::displayBox() {
    m_state->context->Display(m_state->box, AIS_Shaded, -1, false);
    m_state->view->FitAll(0.01, false);
    m_state->paintReady = true;
    m_state->timer->start(kFrameIntervalMs);
}

void ViewerWidget::spinBox() {
    m_state->angle += 0.03;
    if (m_state->angle > kFullTurn) {
        m_state->angle -= kFullTurn;
    }
    gp_Trsf trsf;
    trsf.SetRotation(gp_Ax1(gp_Pnt(50.0, 25.0, 45.0), gp_Dir(0.0, 0.0, 1.0)), m_state->angle);
    m_state->box->SetLocalTransformation(trsf);
    m_state->context->Redisplay(m_state->box, false);
}

void ViewerWidget::releaseScene() {
    if (m_state->timer != nullptr) {
        m_state->timer->stop();
    }
    m_state->box.Nullify();
    m_state->context.Nullify();
    m_state->view.Nullify();
    m_state->viewer.Nullify();
    m_state->paintReady = false;
    m_state->resourcesAlive = false;
    m_state->cleanupConnected = false;
}

#if STEPLENS_USE_WNT_WINDOW

void ViewerWidget::bindWntWindow() {
    if (m_state->view.IsNull() || m_state->paintReady) {
        return;
    }
    try {
        auto* nativePtr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(winId()));
        const auto handle = static_cast<Aspect_Handle>(nativePtr);
        Handle(WNT_Window) window = new WNT_Window(handle);
        m_state->view->SetWindow(window);
        if (!window->IsMapped()) {
            window->Map();
        }
        m_state->view->MustBeResized();
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
    if (m_state->paintReady && !m_state->view.IsNull()) {
        m_state->view->MustBeResized();
        m_state->view->FitAll(0.01, false);
    }
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    if (!m_state->paintReady || m_state->view.IsNull()) {
        return;
    }
    try {
        spinBox();
        m_state->view->Redraw();
    } catch (const Standard_Failure& failure) {
        qWarning("StepLens: WNT redraw failed: %s", steplens::occt::failureMessage(failure));
        m_state->paintReady = false;
        m_state->timer->stop();
    } catch (...) {
        m_state->paintReady = false;
        m_state->timer->stop();
    }
}

#else

void ViewerWidget::bindContextCleanup() {
    if (m_state->cleanupConnected || context() == nullptr) {
        return;
    }
    connect(
        context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { releaseGl(); }, Qt::DirectConnection);
    m_state->cleanupConnected = true;
}

void ViewerWidget::releaseGl() {
    if (!m_state->resourcesAlive) {
        return;
    }
    QOpenGLContext* ctx = context();
    const bool contextValid = ctx != nullptr && ctx->isValid();
    const bool alreadyCurrent = contextValid && QOpenGLContext::currentContext() == ctx;
    const GlCleanupPlan plan = planGlCleanup(GlCleanupInput{true, contextValid, alreadyCurrent});
    bool madeCurrent = false;
    if (plan.makeCurrent) {
        madeCurrent = makeCurrent();
    }
    if (plan.releaseResources) {
        releaseScene();
    }
    if (plan.doneCurrent && madeCurrent) {
        doneCurrent();
    }
}

void ViewerWidget::initializeGL() {
    try {
        if (m_state->view.IsNull()) {
            createScene();
        }
        if (m_state->view.IsNull()) {
            return;
        }
        bindContextCleanup();
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
        m_state->view->SetWindow(window, gl->RenderingContext());
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
        Handle(OpenGl_GraphicDriver)::DownCast(m_state->context->CurrentViewer()->Driver());
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
        if (!m_state->fboWarned) {
            m_state->fboWarned = true;
            // 无 GPU / 离屏时这里会失败。进程继续跑，像素检查留给清单，不在 CI 里判。
            qWarning("StepLens: OpenGl_FrameBuffer::InitWrapper failed");
        }
        return false;
    }
    Handle(Aspect_NeutralWindow) window = Handle(Aspect_NeutralWindow)::DownCast(m_state->view->Window());
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
        m_state->view->MustBeResized();
        m_state->view->FitAll(0.01, false);
        m_state->view->Invalidate();
    }
    return true;
}

void ViewerWidget::paintGL() {
    if (!m_state->paintReady || m_state->view.IsNull() || m_state->context.IsNull()) {
        return;
    }
    try {
        if (!wrapQtFramebuffer()) {
            return;
        }
        spinBox();
        m_state->view->Redraw();
    } catch (const Standard_Failure& failure) {
        qWarning("StepLens: paintGL failed: %s", steplens::occt::failureMessage(failure));
        m_state->paintReady = false;
        m_state->timer->stop();
    } catch (...) {
        m_state->paintReady = false;
        m_state->timer->stop();
    }
}

#endif

} // namespace steplens
