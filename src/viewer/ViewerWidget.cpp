#include "viewer/ViewerWidget.h"

#include "measure/AngleMeasure.h"
#include "measure/DistanceMeasure.h"
#include "occtsupport/occt_compat.h"
#include "pipeline/SceneHost.h"
#include "scene/SceneBuilder.h"
#include "viewer/GlCleanup.h"
#include "viewer/SceneHostBinding.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QMimeData>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <Quantity_Color.hxx>

#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Point.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <AIS_ViewController.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_GradientFillMethod.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <Aspect_ScrollDelta.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CartesianPoint.hxx>
#include <Graphic3d_Camera.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_ImageDumpOptions.hxx>
#include <V3d_TypeOfOrientation.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <optional>
#include <vector>

#if STEPLENS_USE_WNT_WINDOW
#include <WNT_Window.hxx>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32) && !STEPLENS_USE_WNT_WINDOW
// Qt 的头在包含 windows.h 时会定义 NOGDI 和 NOUSER，wingdi.h / winuser.h 里的函数被整段跳过。
// GetModuleHandleA / GetProcAddress 在 libloaderapi.h，不在这段守卫里，不能再声明一遍。
extern "C" {
__declspec(dllimport) HDC __stdcall wglGetCurrentDC(void);
__declspec(dllimport) HWND __stdcall WindowFromDC(HDC);
__declspec(dllimport) void* __stdcall wglGetCurrentContext(void);
__declspec(dllimport) void* __stdcall wglGetProcAddress(const char* name);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
}
#endif

namespace steplens {
namespace {

constexpr int kFrameIntervalMs = 16;

#if !STEPLENS_USE_WNT_WINDOW
// 目录由 STEPLENS_GL_INIT_TRACE 给出。每个进程写 gl-init-<pid>.txt。
// 只读已经 current 的状态。没有 current 就记无 current，不调用会建立 current 的接口。
void traceGlInit(const QOpenGLWidget& widget, const char* site) {
    const QByteArray spec = qgetenv("STEPLENS_GL_INIT_TRACE");
    if (spec.isEmpty() || spec == "0" || (!spec.contains('/') && !spec.contains('\\'))) {
        return;
    }
    QByteArray dir = spec;
    while (dir.endsWith('/') || dir.endsWith('\\')) {
        dir.chop(1);
    }
    const char sep = dir.contains('\\') ? '\\' : '/';
    const QSurfaceFormat requested = widget.format();
    const QOpenGLContext* context = QOpenGLContext::currentContext();
    const bool qtCurrent = context != nullptr;
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
    void* wglContext = wglGetCurrentContext();
    const bool wglCurrent = wglContext != nullptr;
    void* viewportExport = nullptr;
    const void* versionText = nullptr;
    const void* rendererText = nullptr;
    void* useProgram = nullptr;
    void* bindBuffer = nullptr;
    void* enableFn = nullptr;
    if (HMODULE opengl = GetModuleHandleA("opengl32.dll")) {
        viewportExport = reinterpret_cast<void*>(GetProcAddress(opengl, "glViewport"));
        if (wglCurrent) {
            using GetStringFn = const unsigned char*(__stdcall*)(unsigned int);
            auto getString = reinterpret_cast<GetStringFn>(GetProcAddress(opengl, "glGetString"));
            if (getString != nullptr) {
                versionText = getString(0x1F02);
                rendererText = getString(0x1F01);
            }
        }
    }
    if (wglCurrent) {
        useProgram = wglGetProcAddress("glUseProgram");
        bindBuffer = wglGetProcAddress("glBindBuffer");
        enableFn = wglGetProcAddress("glEnable");
    }
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
    void* wglContext = nullptr;
    const bool wglCurrent = false;
    void* viewportExport = nullptr;
    const void* versionText = nullptr;
    const void* rendererText = nullptr;
    void* useProgram = nullptr;
    void* bindBuffer = nullptr;
    void* enableFn = nullptr;
    static_cast<void>(wglContext);
    static_cast<void>(viewportExport);
    static_cast<void>(versionText);
    static_cast<void>(rendererText);
    static_cast<void>(useProgram);
    static_cast<void>(bindBuffer);
    static_cast<void>(enableFn);
#endif
    const QByteArray path =
        dir + sep + "gl-init-" + QByteArray::number(static_cast<qulonglong>(pid)) + ".txt";
    FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.constData(), "ab") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.constData(), "ab");
#endif
    if (file == nullptr) {
        return;
    }
    char actualVersion[32] = "none";
    char actualProfile[16] = "na";
    char actualRenderable[16] = "na";
    if (qtCurrent) {
        std::snprintf(actualVersion, sizeof(actualVersion), "%d.%d", context->format().majorVersion(),
                      context->format().minorVersion());
        std::snprintf(actualProfile, sizeof(actualProfile), "%d",
                      static_cast<int>(context->format().profile()));
        std::snprintf(actualRenderable, sizeof(actualRenderable), "%d",
                      static_cast<int>(context->format().renderableType()));
    }
    auto queriedPointer = [](bool queried, void* value, char* buffer, std::size_t bufferSize) {
        if (!queried) {
            std::snprintf(buffer, bufferSize, "not-queried");
            return;
        }
        std::snprintf(buffer, bufferSize, "%p", value);
    };
    char useBuf[32];
    char bindBuf[32];
    char enableBuf[32];
    queriedPointer(wglCurrent, useProgram, useBuf, sizeof(useBuf));
    queriedPointer(wglCurrent, bindBuffer, bindBuf, sizeof(bindBuf));
    queriedPointer(wglCurrent, enableFn, enableBuf, sizeof(enableBuf));
    std::fprintf(file,
                 "site=%s pid=%lu qt_ctx=%s valid=%d wgl_ctx=%s widget=%d.%d widget_profile=%d "
                 "widget_renderable=%d actual=%s actual_profile=%s actual_renderable=%s "
                 "gl_version=%s gl_renderer=%s query_viewport=GetProcAddress(opengl32) "
                 "viewport_export=%p query_useProgram=%s useProgram=%s query_bindBuffer=%s bindBuffer=%s "
                 "query_enable=%s enable=%s\n",
                 site, pid, qtCurrent ? "present" : "none", qtCurrent && context->isValid() ? 1 : 0,
                 wglCurrent ? "present" : "none", requested.majorVersion(), requested.minorVersion(),
                 static_cast<int>(requested.profile()), static_cast<int>(requested.renderableType()),
                 actualVersion, actualProfile, actualRenderable,
                 !wglCurrent ? "not-queried"
                             : (versionText != nullptr ? static_cast<const char*>(versionText) : "null"),
                 !wglCurrent ? "not-queried"
                             : (rendererText != nullptr ? static_cast<const char*>(rendererText) : "null"),
                 viewportExport, wglCurrent ? "wglGetProcAddress" : "not-queried", useBuf,
                 wglCurrent ? "wglGetProcAddress" : "not-queried", bindBuf,
                 wglCurrent ? "wglGetProcAddress" : "not-queried", enableBuf);
    std::fflush(file);
    std::fclose(file);
}

// CI 软件后端取证，不是能力门。仅 STEPLENS_GL_INIT_TRACE 为目录时追加 gl-backend-<pid>.txt。
// 调用点只有 initializeGL：Qt 保证此时 context 已 current。不 makeCurrent，不改绘制流程。
// 版本和渲染器走当前 context 的 functions()；software 模块只用 HMODULE 观察，缺失不判失败。
void traceQtSoftwareBackend() {
    const QByteArray spec = qgetenv("STEPLENS_GL_INIT_TRACE");
    if (spec.isEmpty() || spec == "0" || (!spec.contains('/') && !spec.contains('\\'))) {
        return;
    }
    QByteArray dir = spec;
    while (dir.endsWith('/') || dir.endsWith('\\')) {
        dir.chop(1);
    }
    const char sep = dir.contains('\\') ? '\\' : '/';
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const QOpenGLContext* context = QOpenGLContext::currentContext();
    QByteArray actualVersion = "no-current";
    QByteArray actualProfile = "no-current";
    QByteArray actualRenderable = "no-current";
    QByteArray versionText = "no-current";
    QByteArray rendererText = "no-current";
    int valid = 0;
    if (context != nullptr) {
        valid = context->isValid() ? 1 : 0;
        const QSurfaceFormat actual = context->format();
        actualVersion =
            QByteArray::number(actual.majorVersion()) + '.' + QByteArray::number(actual.minorVersion());
        actualProfile = QByteArray::number(static_cast<int>(actual.profile()));
        actualRenderable = QByteArray::number(static_cast<int>(actual.renderableType()));
        versionText = "no-functions";
        rendererText = "no-functions";
        if (QOpenGLFunctions* functions = context->functions()) {
            const auto* version = functions->glGetString(GL_VERSION);
            const auto* renderer = functions->glGetString(GL_RENDERER);
            versionText = version != nullptr ? reinterpret_cast<const char*>(version) : "null";
            rendererText = renderer != nullptr ? reinterpret_cast<const char*>(renderer) : "null";
        }
    }
    QByteArray moduleName = "opengl32sw.dll";
    const QByteArray overrideName = qgetenv("QT_OPENGL_DLL");
    if (!overrideName.isEmpty()) {
        moduleName = overrideName;
    }
    int moduleLoaded = 0;
    QByteArray modulePath = "not-loaded";
#if defined(_WIN32)
    const HMODULE softwareModule = GetModuleHandleA(moduleName.constData());
    if (softwareModule != nullptr) {
        moduleLoaded = 1;
        wchar_t pathBuffer[MAX_PATH] = {};
        const DWORD pathLength = GetModuleFileNameW(softwareModule, pathBuffer, MAX_PATH);
        if (pathLength > 0 && pathLength < MAX_PATH) {
            modulePath = QString::fromWCharArray(pathBuffer, static_cast<int>(pathLength)).toUtf8();
        } else {
            modulePath = "loaded-path-unavailable";
        }
    }
#else
    modulePath = "not-windows";
#endif
    const QByteArray path =
        dir + sep + "gl-backend-" + QByteArray::number(static_cast<qulonglong>(pid)) + ".txt";
    FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.constData(), "ab") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.constData(), "ab");
#endif
    if (file == nullptr) {
        return;
    }
    std::fprintf(file,
                 "route=QOpenGLContext::format+functions site=initializeGL pid=%lu qt_ctx=%s valid=%d "
                 "actual=%s actual_profile=%s actual_renderable=%s gl_version=%s gl_renderer=%s "
                 "software_module=%s software_loaded=%d software_path=%s\n",
                 pid, context != nullptr ? "present" : "none", valid, actualVersion.constData(),
                 actualProfile.constData(), actualRenderable.constData(), versionText.constData(),
                 rendererText.constData(), moduleName.constData(), moduleLoaded, modulePath.constData());
    std::fflush(file);
    std::fclose(file);
}
#endif
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
        m_widget.discardDistanceGraphics();
        m_widget.suspendIdlePresentation();
        builder().reset(std::move(model));
        if (m_widget.pointDistanceActive()) {
            builder().setVertexPicking(true);
        } else if (m_widget.angleActive()) {
            builder().setAnglePicking(true);
        }
    }

    void enqueue(mesh::MeshBatch batch) override {
        builder().enqueue(std::move(batch));
    }

    void pump() override {
        builder().pump();
        m_widget.redraw();
    }

    void clear() override {
        m_widget.discardDistanceGraphics();
        releaseBuilder();
        m_widget.restoreIdlePresentation();
    }

    void setVertexPicking(bool enabled) {
        if (m_builder) {
            m_builder->setVertexPicking(enabled);
        }
    }

    void setAnglePicking(bool enabled) {
        if (m_builder) {
            m_builder->setAnglePicking(enabled);
        }
    }

    void setDisplayMode(scene::PrototypePrs::Mode mode) {
        if (m_builder) {
            m_builder->setDisplayMode(mode);
        }
    }

    [[nodiscard]] std::optional<gp_Pnt> pickedVertex() const {
        if (!m_builder) {
            return std::nullopt;
        }
        return m_builder->pickedVertex();
    }

    [[nodiscard]] std::optional<scene::SceneBuilder::PickedElement> pickedElement() const {
        if (!m_builder) {
            return std::nullopt;
        }
        return m_builder->pickedElement();
    }

    void releaseBuilder() {
        m_builder.reset();
        m_context.Nullify();
    }

    void rebind(const opencascade::handle<AIS_InteractiveContext>& context) {
        if (m_builder) {
            m_builder.reset();
        }
        m_context = context;
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

    [[nodiscard]] std::shared_ptr<const model::DocumentModel> document() const {
        return m_builder ? m_builder->document() : nullptr;
    }

    void highlightNode(model::NodeId id) {
        if (m_builder)
            m_builder->highlightNode(id);
    }
    void setNodeVisible(model::NodeId id, bool visible) {
        if (m_builder)
            m_builder->setNodeVisible(id, visible);
    }
    std::optional<model::NodeId> selectedNode() {
        if (!m_builder || m_context.IsNull())
            return std::nullopt;
        return m_builder->selectedNode();
    }

private:
    scene::SceneBuilder& builder();

    ViewerWidget& m_widget;
    opencascade::handle<AIS_InteractiveContext> m_context;
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
    SceneHostBinding binding;
    double angle = 0.0;
    enum class GlInitObserved { NotExecuted, ReturnedTrue, ReturnedFalse, Threw };
    GlInitObserved glInitObserved = GlInitObserved::NotExecuted;
    QString glInitError;
    int glInitPhase = 0;
    bool paintReady = false;
    bool fboWarned = false;
    bool resourcesAlive = false;
    bool cleanupConnected = false;
    bool idle = true;
    bool fitPending = false;
    bool triedronShown = false;
    bool pointDistance = false;
    bool anglePick = false;
    struct PendingElement {
        TopAbs_ShapeEnum type = TopAbs_SHAPE;
        TopoDS_Shape shape;
        gp_Trsf transform;
        gp_Pnt anchor;
    };
    std::optional<PendingElement> pendingElement;
    Handle(AIS_InteractiveObject) pendingMarker;
    std::optional<PendingElement> pendingAngle;
    Handle(AIS_InteractiveObject) pendingAngleMarker;
    struct DistanceMark {
        Handle(AIS_InteractiveObject) line;
        Handle(AIS_InteractiveObject) label;
    };
    std::vector<DistanceMark> marks;
};

scene::SceneBuilder& ViewerSceneHost::builder() {
    if (!m_builder) {
        opencascade::handle<AIS_InteractiveContext> context = m_context;
        if (context.IsNull()) {
            context = m_widget.m_state->context;
        }
        // 等到第一次把模型交进来再绑上下文。构造视图时上下文可能还没有窗口。
        m_builder = std::make_unique<scene::SceneBuilder>(context);
        m_context = context;
        m_builder->setFirstFrameCallback([this] { m_widget.fitAll(); });
        if (m_widget.pointDistanceActive()) {
            m_builder->setVertexPicking(true);
        } else if (m_widget.angleActive()) {
            m_builder->setAnglePicking(true);
        }
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
    // PRD 第 4 节：浅灰到白的渐变。竖直方向，上浅灰、下白。
    m_state->view->SetBgGradientColors(Quantity_Color(0.82, 0.84, 0.86, Quantity_TOC_RGB),
                                       Quantity_Color(1.0, 1.0, 1.0, Quantity_TOC_RGB),
                                       Aspect_GradientFillMethod_Vertical);
    m_state->context = new AIS_InteractiveContext(m_state->viewer);
    m_state->box = new AIS_Shape(BRepPrimAPI_MakeBox(100.0, 50.0, 90.0).Shape());
    m_state->binding.bind(m_state->context.get());
    if (m_state->scene) {
        m_state->scene->rebind(m_state->context);
    }
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
    // 只拆 SceneBuilder，让它在上下文还活着时 RemoveAll。宿主留着，LoadPipeline 的引用不悬空。
    // 文档的最后一份 Close 由 SceneBuilder 析构交给后台，不在这里做。
    if (m_state->scene) {
        m_state->scene->releaseBuilder();
    }
    m_state->binding.releaseScene();
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

QImage imageFromPixMap(const Image_PixMap& pixmap, QString* error) {
    const auto width = static_cast<int>(pixmap.SizeX());
    const auto height = static_cast<int>(pixmap.SizeY());
    if (width <= 0 || height <= 0 || pixmap.IsEmpty()) {
        *error = QStringLiteral("离屏图像是空的");
        return {};
    }
    QImage::Format format = QImage::Format_Invalid;
    bool swapRedBlue = false;
    switch (pixmap.Format()) {
    case Image_Format_RGB:
        format = QImage::Format_RGB888;
        break;
    case Image_Format_BGR:
        format = QImage::Format_BGR888;
        break;
    case Image_Format_RGBA:
        format = QImage::Format_RGBA8888;
        break;
    case Image_Format_BGRA:
        format = QImage::Format_RGBA8888;
        swapRedBlue = true;
        break;
    default:
        *error = QStringLiteral("离屏缓冲的像素格式不能导出");
        return {};
    }
    const int pixelBytes = static_cast<int>(pixmap.SizePixelBytes());
    const int expected = (format == QImage::Format_RGB888 || format == QImage::Format_BGR888) ? 3 : 4;
    if (pixelBytes != expected) {
        *error = QStringLiteral("离屏缓冲的像素字节数与格式不一致");
        return {};
    }
    QImage image(width, height, format);
    if (image.isNull()) {
        *error = QStringLiteral("目标图像分配失败");
        return {};
    }
    const bool topDown = pixmap.IsTopDown();
    for (int y = 0; y < height; ++y) {
        const int sourceY = topDown ? y : (height - 1 - y);
        const auto* source = pixmap.Row(static_cast<Standard_Size>(sourceY));
        auto* dest = image.scanLine(y);
        if (!swapRedBlue) {
            std::memcpy(dest, source, static_cast<std::size_t>(width) * static_cast<std::size_t>(pixelBytes));
        } else {
            for (int x = 0; x < width; ++x) {
                const int at = x * 4;
                dest[at] = source[at + 2];
                dest[at + 1] = source[at + 1];
                dest[at + 2] = source[at];
                dest[at + 3] = source[at + 3];
            }
        }
    }
    return image;
}

QImage ViewerWidget::grabView() {
#if STEPLENS_USE_WNT_WINDOW
    return grab();
#else
    return grabFramebuffer();
#endif
}

#ifndef STEPLENS_GIT_SHA
#define STEPLENS_GIT_SHA "unknown"
#endif

void ViewerWidget::writeFrameGuardObserve(bool viewNull, bool viewerNull, bool paintReady,
                                          bool cameraEvaluated, bool cameraNull) {
    const QByteArray spec = qgetenv("STEPLENS_DUMP_SHOTS");
    if (spec.isEmpty()) {
        return;
    }
    QDir dir(QFile::decodeName(spec));
    if (!dir.mkpath(QStringLiteral("."))) {
        return;
    }
    const QString build = QString::fromUtf8(STEPLENS_GIT_SHA);
    QString safe;
    safe.reserve(build.size());
    for (const QChar ch : build) {
        const char16_t c = ch.unicode();
        const bool ok = (c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') ||
                        c == u'.' || c == u'_' || c == u'-';
        safe.append(ok ? ch : QChar(u'_'));
    }
    const qint64 pid = QCoreApplication::applicationPid();
    const QString path = dir.filePath(QStringLiteral("frame-observe-%1-%2.txt").arg(pid).arg(safe));
    QString camera = QStringLiteral("未求值");
    if (cameraEvaluated) {
        camera = cameraNull ? QStringLiteral("1") : QStringLiteral("0");
    }
    QString initText = QStringLiteral("未执行");
    switch (m_state->glInitObserved) {
    case State::GlInitObserved::NotExecuted:
        initText = QStringLiteral("未执行");
        break;
    case State::GlInitObserved::ReturnedTrue:
        initText = QStringLiteral("返回 true");
        break;
    case State::GlInitObserved::ReturnedFalse:
        initText = QStringLiteral("返回 false");
        break;
    case State::GlInitObserved::Threw:
        initText = QStringLiteral("调用抛出");
        break;
    }
    const QString error = m_state->glInitError.isEmpty() ? QStringLiteral("无") : m_state->glInitError;
    QString body;
    body += QStringLiteral("pid=%1\n").arg(pid);
    body += QStringLiteral("build=%1\n").arg(build);
    body += QStringLiteral("view_null=%1\n").arg(viewNull ? 1 : 0);
    body += QStringLiteral("viewer_null=%1\n").arg(viewerNull ? 1 : 0);
    body += QStringLiteral("paint_ready=%1\n").arg(paintReady ? 1 : 0);
    body += QStringLiteral("camera_null=%1\n").arg(camera);
    body += QStringLiteral("gl_init=%1\n").arg(initText);
    body += QStringLiteral("gl_init_phase=%1\n").arg(m_state->glInitPhase);
    body += QStringLiteral("gl_init_error=%1\n").arg(error);
    body += QStringLiteral("limit=只记录 initializeGL 里已有的那一次 OpenGl_Context::Init。没有再次调用 "
                           "Init、makeCurrent、渲染或清除 GL "
                           "错误。phase 0 且 gl_init 为未执行表示调用还没发生。camera_null "
                           "为未求值表示原短路没有调用 Camera。\n");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(body.toUtf8());
}

ViewerWidget::ViewDump ViewerWidget::renderOffscreen(int scale, bool withAlpha) {
    ViewDump dump;
    if (scale != 1 && scale != 2 && scale != 4) {
        dump.error = QStringLiteral("分辨率只能是 1 倍、2 倍或 4 倍");
        return dump;
    }
    const bool viewNull = m_state->view.IsNull();
    const bool viewerNull = m_state->viewer.IsNull();
    const bool paintReady = m_state->paintReady;
    bool cameraEvaluated = false;
    bool cameraNull = false;
    if (!viewNull && !viewerNull && paintReady) {
        cameraNull = m_state->view->Camera().IsNull();
        cameraEvaluated = true;
    }
    if (viewNull || viewerNull || !paintReady || cameraNull) {
        dump.error = QStringLiteral("当前视图还不能离屏导出");
        writeFrameGuardObserve(viewNull, viewerNull, paintReady, cameraEvaluated, cameraNull);
        return dump;
    }
    const double ratio = static_cast<double>(devicePixelRatioF());
    const qint64 baseWidth = std::llround(static_cast<double>(width()) * ratio);
    const qint64 baseHeight = std::llround(static_cast<double>(height()) * ratio);
    const qint64 targetWidth = baseWidth * static_cast<qint64>(scale);
    const qint64 targetHeight = baseHeight * static_cast<qint64>(scale);
    constexpr qint64 kMaxPixels = std::numeric_limits<int>::max();
    if (baseWidth <= 0 || baseHeight <= 0 || targetWidth > kMaxPixels || targetHeight > kMaxPixels) {
        dump.error = QStringLiteral("目标像素尺寸不能交给离屏接口");
        return dump;
    }
#if !STEPLENS_USE_WNT_WINDOW
    if (context() == nullptr) {
        dump.error = QStringLiteral("视图上下文还没准备好");
        return dump;
    }
    makeCurrent();
#endif
    struct ReleaseContext {
        ViewerWidget* widget;
        ~ReleaseContext() {
#if !STEPLENS_USE_WNT_WINDOW
            widget->doneCurrent();
#endif
            widget->update();
        }
    } releaseContext{this};

    opencascade::handle<OpenGl_GraphicDriver> driver =
        opencascade::handle<OpenGl_GraphicDriver>::DownCast(m_state->viewer->Driver());
    if (driver.IsNull()) {
        dump.error = QStringLiteral("当前视图没有 OpenGL 驱动，不能离屏导出");
        return dump;
    }
    opencascade::handle<Graphic3d_Camera> savedCamera = new Graphic3d_Camera(m_state->view->Camera());
    const bool wasOpaque = driver->Options().buffersOpaqueAlpha;
    struct RestoreView {
        opencascade::handle<V3d_View> view;
        opencascade::handle<Graphic3d_Camera> camera;
        OpenGl_GraphicDriver* driver = nullptr;
        bool opaque = true;
        ~RestoreView() {
            if (driver != nullptr) {
                driver->ChangeOptions().buffersOpaqueAlpha = opaque;
            }
            if (!view.IsNull() && !camera.IsNull()) {
                view->SetCamera(camera);
            }
        }
    } restoreView{m_state->view, savedCamera, driver.get(), wasOpaque};
    if (withAlpha) {
        // 官方清屏：alpha 写入打开时背景 alpha 为 0，渐变背景顶点 alpha 也是 0。
        driver->ChangeOptions().buffersOpaqueAlpha = false;
    }

    Image_PixMap pixmap;
    V3d_ImageDumpOptions params;
    params.Width = static_cast<Standard_Integer>(targetWidth);
    params.Height = static_cast<Standard_Integer>(targetHeight);
    params.BufferType = withAlpha ? Graphic3d_BT_RGBA : Graphic3d_BT_RGB;
    params.ToAdjustAspect = Standard_True;
    bool dumped = false;
    try {
        dumped = m_state->view->ToPixMap(pixmap, params) == Standard_True;
    } catch (const Standard_Failure&) {
        dumped = false;
    }
    if (!dumped || pixmap.IsEmpty()) {
        dump.error = QStringLiteral("离屏渲染失败，没有改用拉伸图");
        return dump;
    }
    if (withAlpha && pixmap.Format() != Image_Format_RGBA && pixmap.Format() != Image_Format_BGRA) {
        dump.error = QStringLiteral("离屏缓冲没有 alpha，透明 PNG 没有完成");
        return dump;
    }
    QString convertError;
    dump.image = imageFromPixMap(pixmap, &convertError);
    if (!convertError.isEmpty() || dump.image.isNull() ||
        dump.image.width() != static_cast<int>(targetWidth) ||
        dump.image.height() != static_cast<int>(targetHeight)) {
        dump.image = QImage();
        dump.error = convertError.isEmpty() ? QStringLiteral("离屏图像尺寸与目标像素不一致") : convertError;
        return dump;
    }
    return dump;
}

void ViewerWidget::highlightNode(model::NodeId id) {
    if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
        host->highlightNode(id);
        redraw();
    }
}

void ViewerWidget::setPartDisplayMode(int mode) {
    if (mode != scene::PrototypePrs::Shaded && mode != scene::PrototypePrs::ShadedWithEdges &&
        mode != scene::PrototypePrs::Wireframe) {
        return;
    }
    if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
        host->setDisplayMode(static_cast<scene::PrototypePrs::Mode>(mode));
        redraw();
    }
}

void ViewerWidget::setNodeVisible(model::NodeId id, bool visible) {
    if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
        host->setNodeVisible(id, visible);
        redraw();
    }
}

std::shared_ptr<const model::DocumentModel> ViewerWidget::document() const {
    if (const auto* host = dynamic_cast<const ViewerSceneHost*>(m_state->scene.get()))
        return host->document();
    return nullptr;
}

void ViewerWidget::mousePressEvent(QMouseEvent* event) {
#if STEPLENS_USE_WNT_WINDOW
    QWidget::mousePressEvent(event);
#else
    QOpenGLWidget::mousePressEvent(event);
#endif
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)
        m_clickGesture.press(event->position().x(), event->position().y());
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
    if (event->button() == Qt::LeftButton &&
        m_clickGesture.isClick(event->position().x(), event->position().y()) && !m_state->context.IsNull() &&
        !m_state->view.IsNull()) {
        m_state->context->MoveTo(point.x(), point.y(), m_state->view, false);
        m_state->context->SelectDetected();
        if (m_state->pointDistance) {
            acceptPointClick();
            return;
        }
        if (m_state->anglePick) {
            acceptAngleClick();
            return;
        }
        if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
            if (const auto id = host->selectedNode())
                emit nodeSelected(*id);
        }
    }
    if (event->button() == Qt::RightButton &&
        m_clickGesture.isClick(event->position().x(), event->position().y()) && !m_state->context.IsNull() &&
        !m_state->view.IsNull() && !m_state->pointDistance && !m_state->anglePick) {
        m_state->context->MoveTo(point.x(), point.y(), m_state->view, false);
        m_state->context->SelectDetected();
        auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get());
        const std::optional<model::NodeId> id = host == nullptr ? std::nullopt : host->selectedNode();
        if (id) {
            emit nodeSelected(*id);
        }
        emit partContextMenu(id.has_value(), id.value_or(0), event->globalPosition().toPoint());
    }
}

void ViewerWidget::mouseMoveEvent(QMouseEvent* event) {
#if STEPLENS_USE_WNT_WINDOW
    QWidget::mouseMoveEvent(event);
#else
    QOpenGLWidget::mouseMoveEvent(event);
#endif
    if (event->buttons().testFlag(Qt::LeftButton) || event->buttons().testFlag(Qt::RightButton))
        m_clickGesture.move(event->position().x(), event->position().y());
    if (!m_state->paintReady) {
        return;
    }
    const Graphic3d_Vec2i point = pixelPoint(event->position(), devicePixelRatioF());
    if (m_state->input.UpdateMousePosition(point, mouseButtons(event->buttons()),
                                           mouseModifiers(event->modifiers()), false)) {
        update();
    }
    if ((m_state->pointDistance || m_state->anglePick) && event->buttons() == Qt::NoButton &&
        !m_state->context.IsNull() && !m_state->view.IsNull()) {
        m_state->context->MoveTo(point.x(), point.y(), m_state->view, false);
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
#if !STEPLENS_USE_WNT_WINDOW
    traceQtSoftwareBackend();
#endif
    struct ExitTrace {
        const QOpenGLWidget* widget;
        ~ExitTrace() {
            traceGlInit(*widget, "initializeGL-exit");
        }
    } exitTrace{this};
    traceGlInit(*this, "initializeGL-enter");
    try {
        if (m_state->view.IsNull()) {
            createScene();
        }
        if (m_state->view.IsNull()) {
            return;
        }
        bindContextCleanup();
        Handle(OpenGl_Context) gl = new OpenGl_Context();
        m_state->glInitPhase = 1;
        const auto initCode = gl->Init(true);
        m_state->glInitPhase = 2;
        if (!initCode) {
            m_state->glInitObserved = State::GlInitObserved::ReturnedFalse;
            m_state->glInitError = QStringLiteral("OpenGl_Context::Init 返回 false");
            qWarning("StepLens: OpenGl_Context::Init failed; 3D smoke stays blank");
            return;
        }
        m_state->glInitObserved = State::GlInitObserved::ReturnedTrue;
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
        const char* message = steplens::occt::failureMessage(failure);
        if (m_state->glInitPhase == 1) {
            m_state->glInitObserved = State::GlInitObserved::Threw;
            m_state->glInitError = QString::fromUtf8(message);
        } else if (m_state->glInitPhase == 0) {
            m_state->glInitError = QStringLiteral("未执行；调用前异常：") + QString::fromUtf8(message);
        }
        qWarning("StepLens: NeutralWindow init failed: %s", message);
    } catch (const std::exception& ex) {
        if (m_state->glInitPhase == 1) {
            m_state->glInitObserved = State::GlInitObserved::Threw;
            m_state->glInitError = QString::fromUtf8(ex.what());
        } else if (m_state->glInitPhase == 0) {
            m_state->glInitError = QStringLiteral("未执行；调用前异常：") + QString::fromUtf8(ex.what());
        }
        qWarning("StepLens: NeutralWindow init failed: %s", ex.what());
    } catch (...) {
        if (m_state->glInitPhase == 1) {
            m_state->glInitObserved = State::GlInitObserved::Threw;
            m_state->glInitError = QStringLiteral("没有异常消息");
        } else if (m_state->glInitPhase == 0) {
            m_state->glInitError = QStringLiteral("未执行；调用前异常没有消息");
        }
        qWarning("StepLens: NeutralWindow init failed");
    }
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    traceGlInit(*this, "paint-before");
    QOpenGLWidget::paintEvent(event);
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

namespace {

Quantity_Color measureColor() {
    return Quantity_Color(0.9, 0.35, 0.05, Quantity_TOC_RGB);
}

void showMeasureObject(const Handle(AIS_InteractiveContext) & context,
                       const Handle(AIS_InteractiveObject) & object) {
    if (context.IsNull() || object.IsNull()) {
        return;
    }
    context->Display(object, 0, -1, false);
    context->Deactivate(object);
}

void eraseMeasureObject(const Handle(AIS_InteractiveContext) & context,
                        const Handle(AIS_InteractiveObject) & object) {
    if (context.IsNull() || object.IsNull()) {
        return;
    }
    context->Remove(object, false);
}

} // namespace

void ViewerWidget::discardDistanceGraphics() {
    eraseMeasureObject(m_state->context, m_state->pendingMarker);
    m_state->pendingMarker.Nullify();
    for (const auto& mark : m_state->marks) {
        eraseMeasureObject(m_state->context, mark.line);
        eraseMeasureObject(m_state->context, mark.label);
    }
    m_state->marks.clear();
    m_state->pendingElement.reset();
    eraseMeasureObject(m_state->context, m_state->pendingAngleMarker);
    m_state->pendingAngleMarker.Nullify();
    m_state->pendingAngle.reset();
}

bool ViewerWidget::pointDistanceActive() const {
    return m_state->pointDistance;
}

void ViewerWidget::beginPointDistance() {
    if (m_state->anglePick) {
        cancelAngle();
    }
    m_state->pointDistance = true;
    m_state->pendingElement.reset();
    eraseMeasureObject(m_state->context, m_state->pendingMarker);
    m_state->pendingMarker.Nullify();
    if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
        host->setVertexPicking(true);
    }
    emit pointDistanceChanged(true);
    emit measureMessage(tr("距离：请选择第一个顶点、边或面"));
    update();
}

void ViewerWidget::cancelPointDistance() {
    if (!m_state->pointDistance) {
        return;
    }
    m_state->pointDistance = false;
    m_state->pendingElement.reset();
    eraseMeasureObject(m_state->context, m_state->pendingMarker);
    m_state->pendingMarker.Nullify();
    if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
        host->setVertexPicking(false);
    }
    emit pointDistanceChanged(false);
    emit measureMessage(tr("已取消距离选择"));
    update();
}

void ViewerWidget::clearDistanceMeasurements() {
    discardDistanceGraphics();
    const QString message = m_state->anglePick       ? tr("测量已清除。请选择第一条直线边或平面")
                            : m_state->pointDistance ? tr("测量已清除。请选择第一个顶点、边或面")
                                                     : tr("测量已清除");
    emit measureMessage(message);
    update();
}

bool ViewerWidget::angleActive() const {
    return m_state->anglePick;
}

void ViewerWidget::beginAngle() {
    if (m_state->pointDistance) {
        cancelPointDistance();
    }
    m_state->anglePick = true;
    m_state->pendingAngle.reset();
    eraseMeasureObject(m_state->context, m_state->pendingAngleMarker);
    m_state->pendingAngleMarker.Nullify();
    if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
        host->setAnglePicking(true);
    }
    emit angleChanged(true);
    emit measureMessage(tr("角度：请选择第一条直线边或平面"));
    update();
}

void ViewerWidget::cancelAngle() {
    if (!m_state->anglePick) {
        return;
    }
    m_state->anglePick = false;
    m_state->pendingAngle.reset();
    eraseMeasureObject(m_state->context, m_state->pendingAngleMarker);
    m_state->pendingAngleMarker.Nullify();
    if (auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get())) {
        host->setAnglePicking(false);
    }
    emit angleChanged(false);
    emit measureMessage(tr("已取消角度选择"));
    update();
}

void ViewerWidget::acceptAngleClick() {
    auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get());
    if (host == nullptr) {
        emit measureMessage(tr("没有点到边或面"));
        return;
    }
    const std::optional<scene::SceneBuilder::PickedElement> picked = host->pickedElement();
    if (!picked || picked->shape.IsNull() ||
        (picked->shape.ShapeType() != TopAbs_EDGE && picked->shape.ShapeType() != TopAbs_FACE)) {
        emit measureMessage(tr("没有点到边或面"));
        return;
    }
    State::PendingElement element;
    element.type = picked->shape.ShapeType();
    element.shape = picked->shape;
    element.transform = picked->transform;
    if (!m_state->pendingAngle) {
        m_state->pendingAngle = element;
        eraseMeasureObject(m_state->context, m_state->pendingAngleMarker);
        opencascade::handle<AIS_Shape> marker = new AIS_Shape(element.shape);
        marker->SetLocalTransformation(element.transform);
        marker->SetColor(measureColor());
        marker->SetWidth(2.0);
        m_state->pendingAngleMarker = marker;
        showMeasureObject(m_state->context, m_state->pendingAngleMarker);
        emit measureMessage(tr("角度：请选择第二条直线边或平面"));
        update();
        return;
    }
    const State::PendingElement first = *m_state->pendingAngle;
    m_state->pendingAngle.reset();
    eraseMeasureObject(m_state->context, m_state->pendingAngleMarker);
    m_state->pendingAngleMarker.Nullify();
    measure::MeasureShape start;
    start.shape = first.shape;
    start.transform = first.transform;
    measure::MeasureShape end;
    end.shape = element.shape;
    end.transform = element.transform;
    const auto result = measure::angleBetween(start, end);
    if (!result.ok()) {
        emit measureMessage(QString::fromUtf8(result.error().userMessage.c_str()));
        update();
        return;
    }
    const QString text = QString::number(result.value().degrees, 'f', 2) + QStringLiteral("°");
    // 放在两次所选形状的包围盒中心，沿用距离标注的文字样式。这个点不是角度顶点，也不画弧。
    Bnd_Box box;
    BRepBndLib::Add(first.shape.Moved(TopLoc_Location(first.transform), Standard_True), box);
    BRepBndLib::Add(element.shape.Moved(TopLoc_Location(element.transform), Standard_True), box);
    gp_Pnt at(0.0, 0.0, 0.0);
    if (!box.IsVoid()) {
        const gp_Pnt low = box.CornerMin();
        const gp_Pnt high = box.CornerMax();
        at = gp_Pnt((low.X() + high.X()) * 0.5, (low.Y() + high.Y()) * 0.5, (low.Z() + high.Z()) * 0.5);
    }
    if (!std::isfinite(at.X()) || !std::isfinite(at.Y()) || !std::isfinite(at.Z())) {
        at = gp_Pnt(0.0, 0.0, 0.0);
    }
    opencascade::handle<AIS_TextLabel> label = new AIS_TextLabel();
    const QByteArray utf8 = text.toUtf8();
    label->SetText(TCollection_ExtendedString(utf8.constData(), Standard_True));
    label->SetPosition(at);
    label->SetColor(measureColor());
    label->SetZoomable(Standard_True);
    double height = 1.0;
    if (const auto document = this->document()) {
        const Bnd_Box& world = document->worldBox();
        if (!world.IsVoid()) {
            const double diagonal = world.CornerMin().Distance(world.CornerMax());
            if (std::isfinite(diagonal) && diagonal > 0.0) {
                height = diagonal * 0.02;
            }
        }
    }
    label->SetHeight(height);
    State::DistanceMark mark;
    mark.label = label;
    showMeasureObject(m_state->context, mark.label);
    m_state->marks.push_back(std::move(mark));
    emit measureMessage(text);
    update();
}

void ViewerWidget::acceptPointClick() {
    auto* host = dynamic_cast<ViewerSceneHost*>(m_state->scene.get());
    if (host == nullptr) {
        emit measureMessage(tr("没有点到顶点、边或面"));
        return;
    }
    const std::optional<scene::SceneBuilder::PickedElement> picked = host->pickedElement();
    if (!picked || picked->shape.IsNull()) {
        emit measureMessage(tr("没有点到顶点、边或面"));
        return;
    }
    State::PendingElement element;
    element.type = picked->shape.ShapeType();
    element.shape = picked->shape;
    element.transform = picked->transform;
    if (element.type == TopAbs_VERTEX) {
        element.anchor = BRep_Tool::Pnt(TopoDS::Vertex(element.shape)).Transformed(element.transform);
    }
    if (!m_state->pendingElement) {
        m_state->pendingElement = element;
        eraseMeasureObject(m_state->context, m_state->pendingMarker);
        if (element.type == TopAbs_VERTEX) {
            opencascade::handle<AIS_Point> marker = new AIS_Point(new Geom_CartesianPoint(element.anchor));
            marker->SetColor(measureColor());
            m_state->pendingMarker = marker;
        } else {
            opencascade::handle<AIS_Shape> marker = new AIS_Shape(element.shape);
            marker->SetLocalTransformation(element.transform);
            marker->SetColor(measureColor());
            marker->SetWidth(2.0);
            m_state->pendingMarker = marker;
        }
        showMeasureObject(m_state->context, m_state->pendingMarker);
        emit measureMessage(tr("距离：请选择第二个顶点、边或面"));
        update();
        return;
    }
    const State::PendingElement first = *m_state->pendingElement;
    m_state->pendingElement.reset();
    eraseMeasureObject(m_state->context, m_state->pendingMarker);
    m_state->pendingMarker.Nullify();
    if (first.type == TopAbs_VERTEX && element.type == TopAbs_VERTEX) {
        commitPointDistance(first.anchor.X(), first.anchor.Y(), first.anchor.Z(), element.anchor.X(),
                            element.anchor.Y(), element.anchor.Z());
        return;
    }
    const auto document = this->document();
    if (!document) {
        emit measureMessage(tr("没有打开的模型"));
        return;
    }
    measure::MeasureShape start;
    start.shape = first.shape;
    start.transform = first.transform;
    measure::MeasureShape end;
    end.shape = element.shape;
    end.transform = element.transform;
    const auto result = measure::distanceBetween(start, end, document->lengthUnitToMm());
    if (!result.ok()) {
        emit measureMessage(QString::fromUtf8(result.error().userMessage.c_str()));
        return;
    }
    showDistanceMark(result.value().millimeters, result.value().pointA.X(), result.value().pointA.Y(),
                     result.value().pointA.Z(), result.value().pointB.X(), result.value().pointB.Y(),
                     result.value().pointB.Z());
}

void ViewerWidget::commitPointDistance(double x0, double y0, double z0, double x1, double y1, double z1) {
    const gp_Pnt first(x0, y0, z0);
    const gp_Pnt second(x1, y1, z1);
    const auto document = this->document();
    if (!document) {
        emit measureMessage(tr("没有打开的模型"));
        return;
    }
    measure::MeasureShape start;
    start.shape = BRepBuilderAPI_MakeVertex(first).Shape();
    measure::MeasureShape end;
    end.shape = BRepBuilderAPI_MakeVertex(second).Shape();
    const auto result = measure::distanceBetween(start, end, document->lengthUnitToMm());
    if (!result.ok()) {
        emit measureMessage(QString::fromUtf8(result.error().userMessage.c_str()));
        return;
    }
    showDistanceMark(result.value().millimeters, result.value().pointA.X(), result.value().pointA.Y(),
                     result.value().pointA.Z(), result.value().pointB.X(), result.value().pointB.Y(),
                     result.value().pointB.Z());
}

void ViewerWidget::showDistanceMark(double millimeters, double ax, double ay, double az, double bx, double by,
                                    double bz) {
    const QString text = QString::number(millimeters, 'f', 3) + QStringLiteral(" mm");
    const gp_Pnt from(ax, ay, az);
    const gp_Pnt to(bx, by, bz);
    State::DistanceMark mark;
    BRepBuilderAPI_MakeEdge edge(from, to);
    if (edge.IsDone()) {
        opencascade::handle<AIS_Shape> line = new AIS_Shape(edge.Shape());
        line->SetColor(measureColor());
        line->SetWidth(2.0);
        mark.line = line;
        showMeasureObject(m_state->context, mark.line);
    }
    opencascade::handle<AIS_TextLabel> label = new AIS_TextLabel();
    const QByteArray utf8 = text.toUtf8();
    label->SetText(TCollection_ExtendedString(utf8.constData(), Standard_True));
    label->SetPosition(
        gp_Pnt((from.X() + to.X()) * 0.5, (from.Y() + to.Y()) * 0.5, (from.Z() + to.Z()) * 0.5));
    label->SetColor(measureColor());
    label->SetZoomable(Standard_True);
    double height = 1.0;
    if (const auto document = this->document()) {
        const Bnd_Box& box = document->worldBox();
        if (!box.IsVoid()) {
            const double diagonal = box.CornerMin().Distance(box.CornerMax());
            if (std::isfinite(diagonal) && diagonal > 0.0) {
                height = diagonal * 0.02;
            }
        }
    }
    label->SetHeight(height);
    mark.label = label;
    showMeasureObject(m_state->context, mark.label);
    m_state->marks.push_back(std::move(mark));
    emit measureMessage(text);
    update();
}

} // namespace steplens
