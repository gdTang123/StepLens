#include "app/MainWindow.h"

#include "app/settings/Settings.h"
#include "core/LifecycleTrace.h"
#include "core/Log.h"
#include "core/MemoryMonitor.h"
#include "core/ProgressSink.h"
#include "core/Version.h"
#include "model/DocumentRelease.h"
#include "pipeline/LoadPipeline.h"
#include "pipeline/SceneHost.h"
#include "ui/AssemblyTreePanel.h"
#include "ui/PropertyPanel.h"
#include "app/FileAssociation.h"
#include "ui/LoadingOverlay.h"
#include "viewer/ViewerWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
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

#include <functional>

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
#include <cstdint>
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

QString screenshotStem(const QString& fileName) {
    QString stem = QFileInfo(fileName).completeBaseName();
    if (stem.isEmpty()) {
        return QStringLiteral("未命名");
    }
    const QString illegal = QStringLiteral("\\/:*?\"<>|");
    for (QChar& character : stem) {
        if (illegal.contains(character) || character.unicode() < 32) {
            character = QLatin1Char('_');
        }
    }
    return stem;
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
    ~NullSceneHost() override {
        model::retireDocument(std::move(m_model));
    }

    void reset(std::shared_ptr<const model::DocumentModel> model) override {
        auto previous = std::move(m_model);
        const bool same = previous && model && previous.get() == model.get();
        m_model = std::move(model);
        if (same)
            model::handoffDocument(std::move(previous));
        else
            model::retireDocument(std::move(previous));
    }
    void enqueue(mesh::MeshBatch batch) override {
        static_cast<void>(batch);
    }
    void pump() override {}
    void clear() override {
        model::retireDocument(std::move(m_model));
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
    connect(escape, &QShortcut::activated, this, [this] {
        if (m_viewer != nullptr && m_viewer->angleActive()) {
            m_viewer->cancelAngle();
            return;
        }
        if (m_viewer != nullptr && m_viewer->pointDistanceActive()) {
            m_viewer->cancelPointDistance();
            return;
        }
        m_pipeline->cancel();
    });
}

MainWindow::~MainWindow() {
    lifecycleTrace("mainwindow-destructor-begin", "MainWindow");
    const auto connectDestroyed = [](QObject* object) {
        const QByteArray identity = object->objectName().isEmpty()
                                        ? QByteArray(object->metaObject()->className())
                                        : object->objectName().toUtf8();
        QObject::connect(object, &QObject::destroyed,
                         [identity] { lifecycleTrace("qobject-child-destroyed", identity.constData()); });
    };
    std::function<void(QObject*)> walk = [&](QObject* object) {
        for (QObject* child : object->children()) {
            connectDestroyed(child);
            walk(child);
        }
    };
    walk(this);
    lifecycleTrace("mainwindow-destructor-end", "members-and-qobject-children-follow");
}

void MainWindow::buildShell() {
    m_selectionLabel = new QLabel(tr("未选择"), this);
    m_selectionLabel->setObjectName(QStringLiteral("selectionLabel"));
    m_unitLabel = new QLabel(this);
    m_unitLabel->setObjectName(QStringLiteral("unitLabel"));
    m_fileLabel = new QLabel(tr("尚未打开文件"), this);
    statusBar()->addPermanentWidget(m_selectionLabel, 1);
    statusBar()->addPermanentWidget(m_unitLabel);
    statusBar()->addPermanentWidget(m_fileLabel);
    statusBar()->showMessage(tr("拖拽 STEP/IGES 文件到这里，或使用文件 - 打开"));

    const bool offscreen = qgetenv("QT_QPA_PLATFORM") == "offscreen";
    if (offscreen) {
        auto* placeholder = new QLabel(tr("离屏模式，未创建三维视图"), this);
        placeholder->setObjectName(QStringLiteral("offscreenPlaceholder"));
        placeholder->setAlignment(Qt::AlignCenter);
        setCentralWidget(placeholder);
        m_nullHost = std::make_unique<NullSceneHost>();
        m_pipeline = std::make_unique<LoadPipeline>(*m_nullHost);
        lifecycleTrace("offscreen-host-created", "NullSceneHost");
        return;
    }

    m_frame = new QWidget(this);
    m_frame->setObjectName(QStringLiteral("viewerFrame"));
    m_viewer = new ViewerWidget(m_frame);
    m_viewer->setObjectName(QStringLiteral("viewerWidget"));
    lifecycleTrace("viewer-created", "viewerWidget");
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

    m_treePanel = new ui::AssemblyTreePanel(this);
    m_treePanel->setObjectName(QStringLiteral("assemblyTreePanel"));
    lifecycleTrace("tree-created", "assemblyTreePanel");
    addDockWidget(Qt::LeftDockWidgetArea, m_treePanel);
    resizeDocks({m_treePanel}, {260}, Qt::Horizontal);
    connect(m_treePanel, &ui::AssemblyTreePanel::nodeActivated, m_viewer, &ViewerWidget::highlightNode);
    connect(m_treePanel, &ui::AssemblyTreePanel::visibilityChanged, m_viewer, &ViewerWidget::setNodeVisible);
    connect(m_viewer, &ViewerWidget::nodeSelected, m_treePanel, &ui::AssemblyTreePanel::selectNode);
    connect(m_viewer, &ViewerWidget::nodeSelected, this, &MainWindow::showSelectedNode);
    connect(m_treePanel, &ui::AssemblyTreePanel::nodeActivated, this, &MainWindow::showSelectedNode);
    connect(m_treePanel, &ui::AssemblyTreePanel::nodeContextMenu, this, [this](model::NodeId id, const QPoint& pos) {
        showPartMenu(true, id, pos);
    });
    connect(m_viewer, &ViewerWidget::partContextMenu, this, &MainWindow::showPartMenu);

    m_propertyPanel = new ui::PropertyPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, m_propertyPanel);
    m_propertyPanel->hide();

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
    auto* recentMenu = fileMenu->addMenu(tr("最近打开的文件"));
    recentMenu->setToolTip(tr("最近 10 个文件"));
    connect(recentMenu, &QMenu::aboutToShow, this, [this, recentMenu] {
        recentMenu->clear();
        const QStringList files = Settings::instance().recentFiles();
        if (files.isEmpty()) {
            recentMenu->addAction(tr("没有最近文件"))->setEnabled(false);
            return;
        }
        for (const QString& path : files) {
            recentMenu->addAction(path, this, [this, path] {
                if (!QFileInfo::exists(path)) {
                    Settings::instance().removeRecentFile(path);
                    QMessageBox::warning(this, tr("无法打开"), tr("文件已不存在，已从最近列表移除"));
                    return;
                }
                openPath(path);
            });
        }
    });
    auto* shot = fileMenu->addAction(tr("导出截图"));
    shot->setEnabled(m_viewer != nullptr);
    shot->setToolTip(tr("把当前三维视图存成 PNG 或 JPG"));
    if (m_viewer != nullptr) {
        connect(shot, &QAction::triggered, this, &MainWindow::exportScreenshot);
    }
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
    auto* displayModes = viewMenu->addMenu(tr("显示模式"));
    auto* displayGroup = new QActionGroup(this);
    displayGroup->setExclusive(true);
    const struct DisplayChoice {
        const char* label;
        int mode;
        bool checked;
    } choices[] = {{"着色+边线", 1, true}, {"仅着色", 0, false}, {"线框", 2, false}};
    for (const DisplayChoice& choice : choices) {
        auto* action = displayModes->addAction(tr(choice.label));
        action->setCheckable(true);
        action->setChecked(choice.checked);
        action->setEnabled(m_viewer != nullptr);
        action->setToolTip(tr(choice.label));
        displayGroup->addAction(action);
        toolbar->addAction(action);
        const int mode = choice.mode;
        connect(action, &QAction::triggered, this, [this, mode] {
            if (m_viewer != nullptr) {
                m_viewer->setPartDisplayMode(mode);
            }
        });
    }
    if (m_treePanel != nullptr) {
        QAction* panels = m_treePanel->toggleViewAction();
        panels->setText(tr("显示或隐藏面板"));
        panels->setToolTip(tr("显示或隐藏装配树"));
        viewMenu->addAction(panels);
    } else {
        auto* panels = viewMenu->addAction(tr("显示或隐藏面板"));
        panels->setEnabled(false);
        panels->setToolTip(tr("当前没有装配树面板"));
    }
    if (m_propertyPanel != nullptr) {
        QAction* properties = m_propertyPanel->toggleViewAction();
        properties->setText(tr("属性"));
        viewMenu->addAction(properties);
    }
    viewMenu->addAction(tr("仅显示此零件"), this, &MainWindow::showOnlyCurrent);
    viewMenu->addAction(tr("全部显示"), this, &MainWindow::showAllParts);

    auto* measureMenu = menuBar()->addMenu(tr("测量"));
    auto* distance = measureMenu->addAction(tr("距离"));
    distance->setCheckable(true);
    distance->setEnabled(m_viewer != nullptr);
    distance->setToolTip(tr("点、边、面的最小距离"));
    auto* angle = measureMenu->addAction(tr("角度"));
    angle->setCheckable(true);
    angle->setEnabled(m_viewer != nullptr);
    angle->setToolTip(tr("直线边或平面的无向较小角，结果 0–90°"));
    auto* clearMeasure = measureMenu->addAction(tr("清除测量"));
    clearMeasure->setEnabled(m_viewer != nullptr);
    clearMeasure->setToolTip(tr("清除测量标注和未完成的选择"));
    if (m_viewer != nullptr) {
        connect(distance, &QAction::toggled, this, [this](bool on) {
            if (on) {
                m_viewer->beginPointDistance();
            } else if (m_viewer->pointDistanceActive()) {
                m_viewer->cancelPointDistance();
            }
        });
        connect(angle, &QAction::toggled, this, [this](bool on) {
            if (on) {
                m_viewer->beginAngle();
            } else if (m_viewer->angleActive()) {
                m_viewer->cancelAngle();
            }
        });
        connect(m_viewer, &ViewerWidget::pointDistanceChanged, distance, &QAction::setChecked);
        connect(m_viewer, &ViewerWidget::angleChanged, angle, &QAction::setChecked);
        connect(m_viewer, &ViewerWidget::measureMessage, statusBar(), &QStatusBar::showMessage);
        connect(clearMeasure, &QAction::triggered, m_viewer, &ViewerWidget::clearDistanceMeasurements);
    }
    toolbar->addAction(distance);
    toolbar->addAction(angle);
    toolbar->addAction(clearMeasure);
    toolbar->addAction(shot);

    auto* helpMenu = menuBar()->addMenu(tr("帮助"));
    helpMenu->addAction(tr("快捷键说明"), this, [this] {
        QMessageBox::information(this, tr("快捷键"),
                                 tr("打开：Ctrl+O\n"
                                    "适应窗口：F\n"
                                    "距离：测量 - 距离，依次选择顶点、边或面\n"
                                    "角度：测量 - 角度，依次选择直线边或平面；状态栏显示无向较小角，0–90°，两位小数\n"
                                    "取消选择：Esc（没有在选距离或角度时，Esc 取消加载）\n"
                                    "清除测量：测量 - 清除测量\n"
                                    "旋转：按住左键拖动\n"
                                    "平移：按住中键拖动\n"
                                    "缩放：滚轮，以光标为中心\n"
                                    "退出：Alt+F4 或文件 - 退出"));
    });
    auto* settingsMenu = menuBar()->addMenu(tr("设置"));
    auto* associate = settingsMenu->addAction(tr("文件关联"));
    associate->setCheckable(true);
    {
        QSignalBlocker blocker(associate);
        associate->setChecked(Settings::instance().associationChosen());
    }
    associate->setToolTip(tr("默认关闭。勾选后登记本程序并打开系统的默认应用选择窗口，不改写现有默认。取消勾选只删本程序仍拥有的登记"));
    connect(associate, &QAction::toggled, this, [this, associate](bool on) {
        const FileAssociationResult result = on ? enableFileAssociation() : disableFileAssociation();
        if (!result.ok) {
            QSignalBlocker blocker(associate);
            associate->setChecked(!on);
            QMessageBox::warning(this, tr("文件关联"), result.message);
            return;
        }
        if (!Settings::instance().setAssociationChosen(on)) {
            QMessageBox::warning(this, tr("文件关联"), tr("关联状态没有写入设置文件"));
        }
    });

    helpMenu->addAction(tr("关于"), this, [this] {
        QMessageBox::about(this, tr("关于 StepLens"),
                           tr("StepLens %1\n内部使用的 STEP/IGES 查看器。\n使用 Qt 与 Open CASCADE。")
                               .arg(QString::fromUtf8(std::string{kVersion}.c_str())));
    });
}

void MainWindow::openPath(const QString& file) {
    const QFileInfo info(file);
    if (!info.exists() || !info.isFile()) {
        showFailure(tr("文件不存在或无法访问"));
        return;
    }
    const QString suffix = info.suffix().toLower();
    const bool isStep = suffix == QStringLiteral("stp") || suffix == QStringLiteral("step");
    const auto bytes = static_cast<std::uint64_t>(info.size());
    auto& settings = Settings::instance();
    const MemorySettings limits = settings.memory();
    const MemoryMonitor monitor;
    const auto sample = monitor.sample();
    const auto estimate = monitor.estimatePeakBytes(bytes, isStep);
    m_memoryNotice.setMemoryConditions(limits.configInvalid, sample.systemAvailPhys == 0);
    if (sample.systemAvailPhys == 0) {
        Log::warn("内存预检采样失败（可用物理内存为 0），继续打开文件");
    }
    const auto decision = MemoryMonitor::preflight(estimate, sample.systemAvailPhys, limits.softLimitRatio,
                                                   limits.hardLimitRatio);
    if (decision == MemoryMonitor::Preflight::Confirm && sample.systemAvailPhys != 0 && m_dumpDir.isEmpty()) {
        const auto answer = QMessageBox::warning(
            this, tr("内存预检"),
            tr("该文件预计需要约 %1 GB 内存，当前可用 %2 GB，继续可能导致系统变慢。\n仍要继续打开吗？")
                .arg(static_cast<double>(estimate) / (1024.0 * 1024 * 1024), 0, 'f', 1)
                .arg(static_cast<double>(sample.systemAvailPhys) / (1024.0 * 1024 * 1024), 0, 'f', 1),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    if (decision == MemoryMonitor::Preflight::Confirm && !m_dumpDir.isEmpty())
        Log::info("自动化模式下跳过内存预检确认，继续打开文件");
    if (sample.systemAvailPhys == 0)
        Log::info("内存预检采样失败，按要求继续打开文件");
    if (limits.configInvalid)
        Log::info("内存比例配置异常，使用默认值继续打开文件");
    m_recentFileRecorded = false;
    const auto pause =
        m_dumpDir.isEmpty() ? std::chrono::milliseconds(0) : std::chrono::milliseconds(kDumpPauseMs);
    m_pipeline->open(pathFromUtf8(file), pause);
    if (!m_closeWhileBusyArmed && qEnvironmentVariableIsSet("STEPLENS_CLOSE_WHILE_BUSY") &&
        !m_pipeline->isSettled()) {
        m_closeWhileBusyArmed = true;
        lifecycleTrace("close-while-busy-request", "after-open");
        close();
        return;
    }
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
    if (m_treePanel != nullptr) {
        // currentModel 是待交付邮箱，adopt 前读它会把 B 的树接到仍是 A 的视口。
        const auto current = m_viewer->document();
        if (current != m_treeDocument) {
            model::handoffDocument(std::move(m_treeDocument));
            m_treeDocument = current;
            m_treePanel->setDocument(current);
            m_hasSelection = false;
            if (m_selectionLabel != nullptr) {
                m_selectionLabel->setText(tr("未选择"));
            }
            if (m_propertyPanel != nullptr) {
                m_propertyPanel->clearNode();
            }
        }
    }
    if (const auto failure = m_pipeline->takeFailure()) {
        const QString message = QString::fromUtf8(failure->userMessage.c_str());
        showFailure(message.isEmpty() ? tr("打开文件时出错") : message);
    }

    const LoadPipeline::Snapshot shot = m_pipeline->snapshot();
    const QString name = fileNameOf(shot.file);
    if (name.isEmpty()) {
        m_fileLabel->setText(tr("尚未打开文件"));
        if (m_unitLabel != nullptr) {
            m_unitLabel->clear();
        }
    } else {
        m_fileLabel->setText(name);
        if (m_unitLabel != nullptr) {
            m_unitLabel->setText(tr("mm"));
        }
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
        if (!m_closeWhileBusyArmed && qEnvironmentVariableIsSet("STEPLENS_CLOSE_WHILE_BUSY")) {
            m_closeWhileBusyArmed = true;
            QTimer::singleShot(0, this, &QWidget::close);
        }
        if (shot.waitingForRelease) {
            statusBar()->showMessage(m_memoryNotice.combine(tr("正在后台释放上一个文件…")));
        } else {
            statusBar()->showMessage(m_memoryNotice.combine(phaseText(shot.progress)));
        }
    } else if (shot.activity == LoadActivity::Ready) {
        m_memoryNotice.finish();
        if (!m_recentFileRecorded && !shot.file.empty()) {
            const std::u8string fileBytes = shot.file.u8string();
            if (!Settings::instance().addRecentFile(
                    QString::fromUtf8(reinterpret_cast<const char*>(fileBytes.data()),
                                      static_cast<qsizetype>(fileBytes.size())))) {
                Log::warn("近期文件记录无法写入设置文件");
            }
            m_recentFileRecorded = true;
        }
        if (!m_memoryNotice.message().isEmpty()) {
            statusBar()->showMessage(m_memoryNotice.message());
        } else {
            statusBar()->showMessage(tr("已完成"));
        }
        if (!m_quitWhenReadyArmed && qEnvironmentVariableIsSet("STEPLENS_QUIT_WHEN_READY")) {
            m_quitWhenReadyArmed = true;
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        }
        if (!m_dumpDir.isEmpty() && !m_modelShotSaved && !m_dumpQuitArmed) {
            m_dumpQuitArmed = true;
            QTimer::singleShot(kModelShotDelayMs, this, &MainWindow::saveModelShots);
        }
    } else if (m_viewer == nullptr || !m_viewer->showingModel()) {
        statusBar()->showMessage(m_memoryNotice.combine(tr("拖拽 STEP/IGES 文件到这里，或使用文件 - 打开")));
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

void MainWindow::exportScreenshot() {
    if (m_viewer == nullptr) {
        statusBar()->showMessage(tr("当前没有三维视图"));
        return;
    }
    QDialog options(this);
    options.setWindowTitle(tr("导出截图"));
    auto* scale = new QComboBox(&options);
    scale->addItem(tr("1 倍"), 1);
    scale->addItem(tr("2 倍"), 2);
    scale->addItem(tr("4 倍"), 4);
    auto* transparent = new QCheckBox(tr("透明背景（仅 PNG）"), &options);
    auto* form = new QFormLayout(&options);
    form->addRow(tr("分辨率"), scale);
    form->addRow(QString(), transparent);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &options);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    if (options.exec() != QDialog::Accepted) {
        return;
    }
    const int factor = scale->currentData().toInt();
    QString model = QStringLiteral("未命名");
    if (m_pipeline) {
        model = screenshotStem(fileNameOf(m_pipeline->snapshot().file));
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString suggested = model + QLatin1Char('_') + stamp + QStringLiteral(".png");
    QString selected = tr("PNG (*.png)");
    const QString chosen = QFileDialog::getSaveFileName(this, tr("导出截图"), suggested,
                                                        tr("PNG (*.png);;JPG (*.jpg *.jpeg)"), &selected);
    if (chosen.isEmpty()) {
        return;
    }
    QString path = chosen;
    QString suffix = QFileInfo(path).suffix().toLower();
    const bool jpgFilter = selected.contains(QStringLiteral("JPG"), Qt::CaseInsensitive);
    if (suffix.isEmpty()) {
        suffix = jpgFilter ? QStringLiteral("jpg") : QStringLiteral("png");
        path += QLatin1Char('.') + suffix;
    }
    const bool png = !(suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"));
    const bool wantAlpha = png && transparent->isChecked();
    const ViewerWidget::ViewDump shot = m_viewer->renderOffscreen(factor, wantAlpha);
    if (!shot.error.isEmpty() || shot.image.isNull()) {
        statusBar()->showMessage(shot.error.isEmpty() ? tr("离屏渲染失败，没有改用拉伸图") : shot.error);
        return;
    }
    if (wantAlpha && !shot.image.hasAlphaChannel()) {
        statusBar()->showMessage(tr("离屏缓冲没有 alpha，透明 PNG 没有完成"));
        return;
    }
    QImage image = wantAlpha ? shot.image : shot.image.convertToFormat(QImage::Format_RGB32);
    const char* format = png ? "PNG" : "JPEG";
    if (!image.save(path, format)) {
        statusBar()->showMessage(tr("截图没有保存"));
        return;
    }
    if (!png && transparent->isChecked()) {
        statusBar()->showMessage(tr("已保存截图。JPG 不包含透明背景"));
        return;
    }
    statusBar()->showMessage(tr("已保存截图"));
}

void MainWindow::showSelectedNode(model::NodeId id) {
    m_hasSelection = true;
    m_selectedId = id;
    QString name;
    if (m_treePanel != nullptr && m_treePanel->treeModel() != nullptr) {
        name = m_treePanel->treeModel()->displayName(id);
    }
    if (m_selectionLabel != nullptr) {
        m_selectionLabel->setText(name.isEmpty() ? tr("未选择") : name);
    }
    if (m_propertyPanel != nullptr && m_treeDocument != nullptr) {
        m_propertyPanel->showNode(*m_treeDocument, id, name);
    }
}

void MainWindow::showPartMenu(bool hasNode, model::NodeId id, QPoint globalPos) {
    if (hasNode) {
        showSelectedNode(id);
    }
    QMenu menu(this);
    auto* only = menu.addAction(tr("仅显示此零件"));
    only->setEnabled(hasNode && m_treePanel != nullptr);
    auto* all = menu.addAction(tr("全部显示"));
    all->setEnabled(m_treePanel != nullptr && m_treePanel->treeModel() != nullptr &&
                    m_treePanel->treeModel()->document() != nullptr);
    connect(only, &QAction::triggered, this, &MainWindow::showOnlyCurrent);
    connect(all, &QAction::triggered, this, &MainWindow::showAllParts);
    menu.exec(globalPos);
}

void MainWindow::showOnlyCurrent() {
    if (!m_hasSelection || m_treePanel == nullptr || m_treePanel->treeModel() == nullptr) {
        return;
    }
    m_treePanel->treeModel()->showOnly(m_selectedId);
}

void MainWindow::showAllParts() {
    if (m_treePanel == nullptr || m_treePanel->treeModel() == nullptr) {
        return;
    }
    m_treePanel->treeModel()->showAll();
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
    lifecycleTrace("end-process-now", "busy-close", 0);
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0);
#else
    std::quick_exit(0);
#endif
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_pipeline && !m_pipeline->isSettled()) {
        lifecycleTrace("mainwindow-closeEvent-busy", "loading-or-releasing");
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
