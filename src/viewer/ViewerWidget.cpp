#include "viewer/ViewerWidget.h"

#include "occtsupport/occt_compat.h"
#include "pipeline/SceneHost.h"
#include "scene/SceneBuilder.h"
#include "viewer/GlCleanup.h"

#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewController.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <Aspect_ScrollDelta.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Standard_Failure.hxx>
#include <V3d_TypeOfOrientation.hxx>
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
constexpr double kFitMargin = 0.01;
constexpr double kTriedronScale = 0.1;

Graphic3d_Vec2i pixelPoint(const QPointF& point, qreal ratio) {
    const double scale = static_cast<double>(ratio);
    return Graphic3d_Vec2i(static_cast<int>(std::llround(point.x() * scale)),
                           static_cast<int>(std::llround(point.y() * scale)));
}

Aspect_VKeyMouse mouseButtons(Qt::MouseButtons buttons) {
    Aspect_VKeyMouse keys = Aspect_VKeyMouse_NONE;
    if ((buttons & Qt::LeftButton) != 0) {
        keys |= Aspect_VKeyMouse_LeftButton;
    }
    if ((buttons & Qt::MiddleButton) != 0) {
        keys |= Aspect_VKeyMouse_MiddleButton;
    }
    if ((buttons & Qt::RightButton) != 0) {
        keys |= Aspect_VKeyMouse_RightButton;
    }
    return keys;
}

Aspect_VKeyFlags mouseModifiers(Qt::KeyboardModifiers modifiers) {
    Aspect_VKeyFlags flags = Aspect_VKeyFlags_NONE;
    if ((modifiers & Qt::ShiftModifier) != 0) {
        flags |= Aspect_VKeyFlags_SHIFT;
    }
    if ((modifiers & Qt::ControlModifier) != 0) {
        flags |= Aspect_VKeyFlags_CTRL;
    }
    if ((modifiers & Qt::AltModifier) != 0) {
        flags |= Aspect_VKeyFlags_ALT;
    }
    return flags;
}

QStringList localFiles(const QMimeData* mime) {
    QStringList files;
    if (mime == nullptr || !mime->hasUrls()) {
        return files;
    }
    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            files.push_back(url.toLocalFile());
        }
    }
    return files;
}

V3d_TypeOfOrientation orientationOf(StandardView view) {
    switch (view) {
    case StandardView::Front:
        return V3d_Yneg;
    case StandardView::Back:
        return V3d_Ypos;
    case StandardView::Left:
        return V3d_Xneg;
    case StandardView::Right:
        return V3d_Xpos;
    case StandardView::Top:
        return V3d_Zpos;
    case StandardView::Bottom:
        return V3d_Zneg;
    case StandardView::Isometric:
        return V3d_XposYnegZpos;
    }
    return V3d_XposYnegZpos;
}

} // namespace

// 默认键位已经是左键旋转、中键平移、滚轮缩放。这里再 Bind 一次，避免以后 OCCT 改默认值。
class OrbitInput final : public AIS_ViewController {
public:
    OrbitInput() {
        ChangeMouseGestureMap().Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_RotateOrbit);
        ChangeMouseGestureMap().Bind(Aspect_VKeyMouse_MiddleButton, AIS_MouseGesture_Pan);
    }

    bool flush(const Handle(AIS_InteractiveContext) & context, const Handle(V3d_View) & view) {
        if (context.IsNull() || view.IsNull()) {
            return false;
        }
        FlushViewEvents(context, view, true);
        const bool again = toAskNextFrame();
        if (again) {
            setAskNextFrame(false);
        }
        return again;
    }
};

class ViewerSceneHost final : public SceneHost {
public:
    explicit ViewerSceneHost(ViewerWidget& widget) : m_widget(widget) {}

    void reset(std::shared_ptr<const model::DocumentModel> model) override {
        m_widget.suspendIdlePresentation();
        builder().reset(std::move(model));
    }

    void enqueue(mesh::MeshBatch batch) override {
        builder().enqueue(std::move(batch));
    }

    void pump() override {
        builder().pump();
        m_widget.redraw();
    }

    void clear() override {
        if (m_builder) {
            m_builder->reset(nullptr);
        }
        m_widget.restoreIdlePresentation();
    }

    void fitAll() override {
        m_widget.fitAll();
    }

    [[nodiscard]] int pending() const override {
        if (!m_builder) {
            return 0;
        }
        return m_builder->pending();
    }

private:
    scene::SceneBuilder& builder();

    ViewerWidget& m_widget;
    std::unique_ptr<scene::SceneBuilder> m_builder;
};

struct ViewerWidget::State {
    QTimer* timer = nullptr;
    Handle(V3d_Viewer) viewer;
    Handle(V3d_View) view;
    Handle(AIS_InteractiveContext) context;
    Handle(AIS_Shape) box;
    OrbitInput input;
    std::unique_ptr<ViewerSceneHost> scene;
    double angle = 0.0;
    bool paintReady = false;
    bool fboWarned = false;
    bool resourcesAlive = false;
    bool cleanupConnected = false;
    bool idle = true;
    bool fitPending = false;
    bool triedronShown = false;
};

scene::SceneBuilder& ViewerSceneHost::builder() {
    if (!m_builder) {
        // 等到第一次把模型交进来再绑上下文。构造视图时上下文可能还没有窗口。
        m_builder = std::make_unique<scene::SceneBuilder>(m_widget.m_state->context);
        m_builder->setFirstFrameCallback([this] { m_widget.fitAll(); });
    }
    return *m_builder;
}

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
    m_state->scene = std::make_unique<ViewerSceneHost>(*this);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
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
    if (m_state->context.IsNull() || m_state->box.IsNull()) {
        return;
    }
    m_state->idle = true;
    m_state->context->Display(m_state->box, AIS_Shaded, -1, false);
    showTriedron();
    if (!m_state->view.IsNull()) {
        m_state->view->FitAll(kFitMargin, false);
    }
    m_state->paintReady = true;
    m_state->timer->start(kFrameIntervalMs);
}

void ViewerWidget::spinBox() {
    if (m_state->box.IsNull() || m_state->context.IsNull()) {
        return;
    }
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
    // SceneBuilder 析构会 RemoveAll。这时上下文还在，不能先把它丢掉。
    m_state->scene.reset();
    m_state->box.Nullify();
    m_state->context.Nullify();
    m_state->view.Nullify();
    m_state->viewer.Nullify();
    m_state->paintReady = false;
    m_state->resourcesAlive = false;
    m_state->cleanupConnected = false;
    m_state->idle = true;
    m_state->fitPending = false;
    m_state->triedronShown = false;
}

void ViewerWidget::showTriedron() {
    if (m_state->triedronShown || m_state->view.IsNull()) {
        return;
    }
    m_state->view->TriedronDisplay(Aspect_TOTP_RIGHT_LOWER, Quantity_NOC_WHITE, kTriedronScale,
                                   V3d_WIREFRAME);
    m_state->triedronShown = true;
}

void ViewerWidget::suspendIdlePresentation() {
    m_state->idle = false;
}

void ViewerWidget::restoreIdlePresentation() {
    m_state->idle = true;
    m_state->fitPending = false;
    if (m_state->context.IsNull() || m_state->view.IsNull()) {
        return;
    }
    if (m_state->box.IsNull()) {
        m_state->box = new AIS_Shape(BRepPrimAPI_MakeBox(100.0, 50.0, 90.0).Shape());
    }
    m_state->context->Display(m_state->box, AIS_Shaded, -1, false);
    showTriedron();
    m_state->view->FitAll(kFitMargin, false);
    m_state->paintReady = true;
    if (m_state->timer != nullptr && !m_state->timer->isActive()) {
        m_state->timer->start(kFrameIntervalMs);
    }
    update();
}

void ViewerWidget::drawScene() {
    if (m_state->fitPending && !m_state->view.IsNull()) {
        m_state->view->FitAll(kFitMargin, false);
        m_state->fitPending = false;
    }
    if (m_state->idle && !m_state->box.IsNull()) {
        spinBox();
    }
    const bool again = m_state->input.flush(m_state->context, m_state->view);
    if (!m_state->view.IsNull()) {
        m_state->view->Redraw();
    }
    noteRedrawRequest(again);
}

void ViewerWidget::noteRedrawRequest(bool again) {
    if (m_state->timer == nullptr) {
        return;
    }
    const bool keep = m_state->idle || again;
    if (keep) {
        if (!m_state->timer->isActive()) {
            m_state->timer->start(kFrameIntervalMs);
        }
    } else if (m_state->timer->isActive()) {
        m_state->timer->stop();
    }
}

void ViewerWidget::fitAll() {
    if (m_state->view.IsNull() || !m_state->paintReady) {
        m_state->fitPending = true;
        return;
    }
    m_state->view->FitAll(kFitMargin, false);
    m_state->view->Invalidate();
    m_state->fitPending = false;
    update();
}

void ViewerWidget::setStandardView(StandardView view) {
    if (m_state->view.IsNull() || !m_state->paintReady) {
        return;
    }
    m_state->input.UpdateViewOrientation(orientationOf(view), true);
    m_state->input.flush(m_state->context, m_state->view);
    update();
}

void ViewerWidget::redraw() {
    update();
}

bool ViewerWidget::showingModel() const {
    return !m_state->idle && m_state->paintReady;
}

SceneHost& ViewerWidget::sceneHost() {
    return *m_state->scene;
}

QImage ViewerWidget::grabView() {
#if STEPLENS_USE_WNT_WINDOW
    return grab();
#else
    return grabFramebuffer();
#endif
}

void ViewerWidget::mousePressEvent(QMouseEvent* event) {
#if STEPLENS_USE_WNT_WINDOW
    QWidget::mousePressEvent(event);
#else
    QOpenGLWidget::mousePressEvent(event);
#endif
    if (!m_state->paintReady) {
        return;
    }
    const Graphic3d_Vec2i point = pixelPoint(event->position(), devicePixelRatioF());
    if (m_state->input.UpdateMouseButtons(point, mouseButtons(event->buttons()),
                                          mouseModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent* event) {
#if STEPLENS_USE_WNT_WINDOW
    QWidget::mouseReleaseEvent(event);
#else
    QOpenGLWidget::mouseReleaseEvent(event);
#endif
    if (!m_state->paintReady) {
        return;
    }
    const Graphic3d_Vec2i point = pixelPoint(event->position(), devicePixelRatioF());
    if (m_state->input.UpdateMouseButtons(point, mouseButtons(event->buttons()),
                                          mouseModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewerWidget::mouseMoveEvent(QMouseEvent* event) {
#if STEPLENS_USE_WNT_WINDOW
    QWidget::mouseMoveEvent(event);
#else
    QOpenGLWidget::mouseMoveEvent(event);
#endif
    if (!m_state->paintReady) {
        return;
    }
    const Graphic3d_Vec2i point = pixelPoint(event->position(), devicePixelRatioF());
    if (m_state->input.UpdateMousePosition(point, mouseButtons(event->buttons()),
                                           mouseModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewerWidget::wheelEvent(QWheelEvent* event) {
#if STEPLENS_USE_WNT_WINDOW
    QWidget::wheelEvent(event);
#else
    QOpenGLWidget::wheelEvent(event);
#endif
    if (!m_state->paintReady) {
        return;
    }
    const Graphic3d_Vec2i point = pixelPoint(event->position(), devicePixelRatioF());
    const double delta = static_cast<double>(event->angleDelta().y()) / 8.0;
    if (m_state->input.UpdateZoom(Aspect_ScrollDelta(point, delta, mouseModifiers(event->modifiers())))) {
        update();
    }
    event->accept();
}

void ViewerWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (!localFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void ViewerWidget::dropEvent(QDropEvent* event) {
    const QStringList files = localFiles(event->mimeData());
    if (files.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    emit filesDropped(files);
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
        if (m_state->idle || m_state->fitPending) {
            m_state->view->FitAll(kFitMargin, false);
            m_state->fitPending = false;
        }
    }
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    if (!m_state->paintReady || m_state->view.IsNull()) {
        return;
    }
    try {
        drawScene();
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
        // QOpenGLWidget::makeCurrent() 返回 void。调完再看是不是已经 current。
        makeCurrent();
        madeCurrent = QOpenGLContext::currentContext() == ctx;
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
        const auto nativeBits = static_cast<std::uintptr_t>(winId());
#if defined(_WIN32)
        // Windows 上 Aspect_Drawable 是 HWND。Linux 上是 XID（整数），不能从 void* 再转一次。
        auto* nativePtr = reinterpret_cast<void*>(nativeBits);
        Aspect_Drawable native = static_cast<Aspect_Drawable>(nativePtr);
#else
        Aspect_Drawable native = static_cast<Aspect_Drawable>(nativeBits);
#endif
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
    const Handle(OpenGl_Context)& gl = driver->GetSharedContext();
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
        if (m_state->idle || m_state->fitPending) {
            m_state->view->FitAll(kFitMargin, false);
            m_state->fitPending = false;
        }
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
        drawScene();
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
