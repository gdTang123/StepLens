#include "app/MainWindow.h"

#include "core/ProgressSink.h"
#include "core/Version.h"
#include "pipeline/LoadPipeline.h"
#include "pipeline/SceneHost.h"
#include "ui/LoadingOverlay.h"
#include "viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

namespace steplens {
namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
constexpr int kUiTickMs = 16;
constexpr int kModelShotDelayMs = 200;
constexpr int kDumpPauseMs = 500;
constexpr int kHintMargin = 24;

std::filesystem::path pathFromUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(bytes.constData()),
                                               static_cast<std::size_t>(bytes.size())));
}

QString fileNameOf(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    const std::u8string bytes = path.filename().u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
}

QString elapsedText(double seconds) {
    if (!(seconds > 0.0)) {
        seconds = 0.0;
    }
    const int total = static_cast<int>(seconds);
    const int minutes = total / 60;
    const int secs = total % 60;
    return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(secs, 2, 10, QLatin1Char('0'));
}

QString phaseText(ProgressPhase phase) {
    const std::string_view label = progressPhaseLabel(phase);
    if (label.empty()) {
        return QStringLiteral("读取文件");
    }
    return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
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

const char* viewName(StandardView view) {
    switch (view) {
    case StandardView::Front:
        return "前视";
    case StandardView::Back:
        return "后视";
    case StandardView::Left:
        return "左视";
    case StandardView::Right:
        return "右视";
    case StandardView::Top:
        return "上视";
    case StandardView::Bottom:
        return "下视";
    case StandardView::Isometric:
        return "等轴测";
    }
    return "等轴测";
}

} // namespace

class MainWindow::NullSceneHost final : public SceneHost {
public:
    void reset(std::shared_ptr<const model::DocumentModel> model) override {
        m_model = std::move(model);
    }
    void enqueue(mesh::MeshBatch batch) override {
        static_cast<void>(batch);
    }
    void pump() override {}
    void clear() override {
        m_model.reset();
    }
    void fitAll() override {}
    [[nodiscard]] int pending() const override {
        return 0;
    }

private:
    std::shared_ptr<const model::DocumentModel> m_model;
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_dumpDir = qEnvironmentVariable("STEPLENS_DUMP_SHOTS");
    setWindowTitle(tr("StepLens"));
    resize(kDefaultWidth, kDefaultHeight);
    setAcceptDrops(true);
    buildShell();
    buildMenus();

    auto* tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &MainWindow::refreshUi);
    tick->start(kUiTickMs);

    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] { m_pipeline->cancel(); });
}

MainWindow::~MainWindow() = default;

void MainWindow::buildShell() {
    m_fileLabel = new QLabel(tr("尚未打开文件"), this);
    statusBar()->addPermanentWidget(m_fileLabel);
    statusBar()->showMessage(tr("拖拽 STEP/IGES 文件到这里，或使用文件 - 打开"));

    const bool offscreen = qgetenv("QT_QPA_PLATFORM") == "offscreen";
    if (offscreen) {
        auto* placeholder = new QLabel(tr("离屏模式，未创建三维视图"), this);
        placeholder->setAlignment(Qt::AlignCenter);
        setCentralWidget(placeholder);
        m_nullHost = std::make_unique<NullSceneHost>();
        m_pipeline = std::make_unique<LoadPipeline>(*m_nullHost);
        return;
    }

    m_frame = new QWidget(this);
    m_viewer = new ViewerWidget(m_frame);
    m_hint = new QLabel(tr("拖拽 STEP/IGES 文件到这里，或点击打开"), m_frame);
    m_hint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_hint->setAutoFillBackground(true);
    QPalette hintPalette = m_hint->palette();
    hintPalette.setColor(QPalette::Window, QColor(0, 0, 0, 120));
    hintPalette.setColor(QPalette::WindowText, QColor(242, 244, 248));
    m_hint->setPalette(hintPalette);
    m_hint->setMargin(8);
    m_overlay = new LoadingOverlay(m_frame);
    m_overlay->hide();
    setCentralWidget(m_frame);
    m_pipeline = std::make_unique<LoadPipeline>(m_viewer->sceneHost());

    connect(m_viewer, &ViewerWidget::filesDropped, this, &MainWindow::openFiles);
    connect(m_overlay, &LoadingOverlay::filesDropped, this, &MainWindow::openFiles);
    connect(m_overlay, &LoadingOverlay::cancelRequested, this, [this] { m_pipeline->cancel(); });
    m_frame->installEventFilter(this);
    layoutView();
}

void MainWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("文件"));
    auto* openAction = fileMenu->addAction(tr("打开..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("打开"), QString(),
            tr("STEP/IGES (*.stp *.step *.igs *.iges);;STEP (*.stp *.step);;IGES (*.igs *.iges)"));
        if (!path.isEmpty()) {
            openPath(path);
        }
    });
    auto* recent = fileMenu->addAction(tr("最近打开的文件"));
    recent->setEnabled(false);
    recent->setToolTip(tr("最近文件要等设置模块"));
    auto* shot = fileMenu->addAction(tr("导出截图"));
    shot->setEnabled(false);
    shot->setToolTip(tr("截图还没做"));
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(tr("退出"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* viewMenu = menuBar()->addMenu(tr("视图"));
    auto* standards = viewMenu->addMenu(tr("标准视图"));
    const StandardView views[] = {StandardView::Front,    StandardView::Back, StandardView::Left,
                                  StandardView::Right,    StandardView::Top,  StandardView::Bottom,
                                  StandardView::Isometric};
    auto* toolbar = addToolBar(tr("工具"));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addAction(openAction);
    auto* fitAction = new QAction(tr("适应窗口"), this);
    fitAction->setShortcut(Qt::Key_F);
    fitAction->setToolTip(tr("适应窗口"));
    fitAction->setEnabled(m_viewer != nullptr);
    connect(fitAction, &QAction::triggered, this, [this] {
        if (m_viewer != nullptr) {
            m_viewer->fitAll();
        }
    });
    viewMenu->addAction(fitAction);
    toolbar->addAction(fitAction);
    for (const StandardView view : views) {
        const QString label = QString::fromUtf8(viewName(view));
        auto* action = standards->addAction(label);
        action->setEnabled(m_viewer != nullptr);
        action->setToolTip(label);
        connect(action, &QAction::triggered, this, [this, view] {
            if (m_viewer != nullptr) {
                m_viewer->setStandardView(view);
            }
        });
        toolbar->addAction(action);
    }
    auto* displayMode = viewMenu->addAction(tr("显示模式"));
    displayMode->setEnabled(false);
    displayMode->setToolTip(tr("显示模式还没做"));
    auto* panels = viewMenu->addAction(tr("显示或隐藏面板"));
    panels->setEnabled(false);
    panels->setToolTip(tr("装配树和属性面板还没做"));

    auto* measureMenu = menuBar()->addMenu(tr("测量"));
    for (const char* name : {"距离", "角度", "清除测量"}) {
        auto* action = measureMenu->addAction(QString::fromUtf8(name));
        action->setEnabled(false);
        action->setToolTip(tr("测量还没做"));
    }

    auto* helpMenu = menuBar()->addMenu(tr("帮助"));
    helpMenu->addAction(tr("快捷键说明"), this, [this] {
        QMessageBox::information(this, tr("快捷键"),
                                 tr("打开：Ctrl+O\n"
                                    "适应窗口：F\n"
                                    "取消加载：Esc\n"
                                    "旋转：按住左键拖动\n"
                                    "平移：按住中键拖动\n"
                                    "缩放：滚轮，以光标为中心\n"
                                    "退出：Alt+F4 或文件 - 退出"));
    });
    helpMenu->addAction(tr("关于"), this, [this] {
        QMessageBox::about(this, tr("关于 StepLens"),
                           tr("StepLens %1\n内部使用的 STEP/IGES 查看器。\n使用 Qt 与 Open CASCADE。")
                               .arg(QString::fromUtf8(std::string{kVersion}.c_str())));
    });
}

void MainWindow::openPath(const QString& file) {
    const auto pause =
        m_dumpDir.isEmpty() ? std::chrono::milliseconds(0) : std::chrono::milliseconds(kDumpPauseMs);
    m_pipeline->open(pathFromUtf8(file), pause);
    refreshUi();
}

void MainWindow::openFiles(const QStringList& files) {
    if (files.isEmpty()) {
        return;
    }
    if (files.size() > 1) {
        statusBar()->showMessage(tr("一次只打开一个文件，已打开第一个"));
    }
    openPath(files.front());
}

void MainWindow::refreshUi() {
    if (!m_pipeline) {
        return;
    }
    m_pipeline->tick();
    if (const auto failure = m_pipeline->takeFailure()) {
        const QString message = QString::fromUtf8(failure->userMessage.c_str());
        showFailure(message.isEmpty() ? tr("打开文件时出错") : message);
    }

    const LoadPipeline::Snapshot shot = m_pipeline->snapshot();
    const QString name = fileNameOf(shot.file);
    if (name.isEmpty()) {
        m_fileLabel->setText(tr("尚未打开文件"));
    } else {
        m_fileLabel->setText(name);
        setWindowTitle(tr("StepLens — %1").arg(name));
    }

    const bool loading = shot.activity == LoadActivity::Loading || shot.activity == LoadActivity::Releasing;
    if (m_overlay != nullptr) {
        if (loading) {
            const QString title =
                shot.waitingForRelease ? tr("正在后台释放上一个文件…") : phaseText(shot.progress);
            QString detail = shot.waitingForRelease ? QString() : elapsedText(shot.elapsedSeconds);
            if (!shot.waitingForRelease) {
                const int percent = static_cast<int>(shot.fraction * 100.f + 0.5f);
                detail = tr("%1%    已用 %2").arg(percent).arg(elapsedText(shot.elapsedSeconds));
                if (shot.stalled || shot.overEstimate) {
                    detail.append(tr("\n仍在处理（已用时 %1）").arg(elapsedText(shot.elapsedSeconds)));
                }
            }
            m_overlay->setStatus(title, detail, shot.fraction, shot.waitingForRelease,
                                 !shot.waitingForRelease && shot.activity == LoadActivity::Loading);
            m_overlay->show();
            m_overlay->raise();
            if (!m_dumpDir.isEmpty() && !m_overlayShotSaved) {
                m_overlayShotSaved = true;
                QTimer::singleShot(kUiTickMs, this, &MainWindow::saveOverlayShot);
            }
        } else {
            m_overlay->hide();
        }
    }
    if (m_hint != nullptr) {
        const bool showHint = m_viewer != nullptr && !m_viewer->showingModel() &&
                              (m_overlay == nullptr || !m_overlay->isVisible());
        m_hint->setVisible(showHint);
    }

    if (shot.activity == LoadActivity::Loading || shot.activity == LoadActivity::Releasing) {
        if (shot.waitingForRelease) {
            statusBar()->showMessage(tr("正在后台释放上一个文件…"));
        } else {
            statusBar()->showMessage(phaseText(shot.progress));
        }
    } else if (shot.activity == LoadActivity::Ready) {
        statusBar()->showMessage(tr("已完成"));
        if (!m_dumpDir.isEmpty() && !m_modelShotSaved && !m_dumpQuitArmed) {
            m_dumpQuitArmed = true;
            QTimer::singleShot(kModelShotDelayMs, this, &MainWindow::saveModelShots);
        }
    } else if (m_viewer == nullptr || !m_viewer->showingModel()) {
        statusBar()->showMessage(tr("拖拽 STEP/IGES 文件到这里，或使用文件 - 打开"));
        setWindowTitle(tr("StepLens"));
    }
    layoutView();
}

void MainWindow::layoutView() {
    if (m_frame == nullptr || m_viewer == nullptr) {
        return;
    }
    const QRect area = m_frame->rect();
    m_viewer->setGeometry(area);
    if (m_overlay != nullptr) {
        m_overlay->setGeometry(area);
    }
    if (m_hint != nullptr) {
        m_hint->adjustSize();
        const int x = std::max(0, (area.width() - m_hint->width()) / 2);
        const int y = std::max(0, area.height() - m_hint->height() - kHintMargin);
        m_hint->move(x, y);
        m_hint->raise();
    }
    if (m_overlay != nullptr && m_overlay->isVisible()) {
        m_overlay->raise();
    }
}

void MainWindow::showFailure(const QString& message) {
    statusBar()->showMessage(message);
    QMessageBox::warning(this, tr("无法打开"), message);
    if (!m_dumpDir.isEmpty()) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
}

void MainWindow::saveOverlayShot() {
    if (m_dumpDir.isEmpty()) {
        return;
    }
    QDir().mkpath(m_dumpDir);
    const QImage image = grab().toImage();
    image.save(QDir(m_dumpDir).filePath(QStringLiteral("overlay.png")));
}

void MainWindow::saveModelShots() {
    if (m_dumpDir.isEmpty() || m_viewer == nullptr) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
    m_modelShotSaved = true;
    if (m_viewer->showingModel()) {
        m_viewer->fitAll();
        m_viewer->redraw();
    }
    QDir().mkpath(m_dumpDir);
    const QImage view = m_viewer->grabView();
    view.save(QDir(m_dumpDir).filePath(QStringLiteral("model.png")));
    const QImage window = grab().toImage();
    window.save(QDir(m_dumpDir).filePath(QStringLiteral("window.png")));
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

void MainWindow::endProcessNow() {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0);
#else
    std::quick_exit(0);
#endif
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_pipeline && !m_pipeline->isSettled()) {
        event->ignore();
        endProcessNow();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!localFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QStringList files = localFiles(event->mimeData());
    if (files.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    openFiles(files);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_frame && event->type() == QEvent::Resize) {
        layoutView();
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace steplens
