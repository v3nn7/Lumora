#include "editor/EditorWindow.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/Snapshot.hpp"
#include "common/Theme.hpp"
#include "common/VulkanViewport.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include "editor/ConsolePanel.hpp"
#include "editor/ContentBrowserPanel.hpp"
#include "editor/EditorViewportClient.hpp"
#include "editor/EntityFactory.hpp"
#include "editor/InspectorPanel.hpp"
#include "editor/ProfilerPanel.hpp"
#include "editor/SceneHierarchyPanel.hpp"
#include "project/ProjectTemplates.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"
#include "runtime/Simulation.hpp"
#include "scene/SceneSerializer.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {

QDockWidget* makeDock(QMainWindow* window, const QString& title, const QString& objectName, QWidget* content) {
    auto* dock = new QDockWidget(title, window);
    dock->setObjectName(objectName);
    dock->setWidget(content);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                      QDockWidget::DockWidgetClosable);
    return dock;
}

} // namespace

EditorWindow::EditorWindow(app::AppContext& app, Project project, QWidget* parent)
    : QMainWindow(parent), app_(app), project_(std::move(project)), client_(std::make_unique<EditorViewportClient>(ctx_)) {
    setWindowIcon(app::theme::icon(QStringLiteral("logo")));
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AnimatedDocks);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    resize(1600, 960);

    client_->onSimulate = [this](float dt) {
        if (simulation_ && !paused_) simulation_->advance(dt);
    };
    client_->onShortcut = [this](int key, Qt::KeyboardModifiers mods) { return handleShortcut(key, mods); };

    buildActions();
    buildCentral();
    buildDocks();
    buildMenus();
    buildToolbar();
    buildStatusBar();
    defaultLayout_ = saveState();
    restoreLayout();

    connect(&ctx_, &EditorContext::dirtyChanged, this, &EditorWindow::updateTitle);

    // Load the project's default scene (or start a fresh one if it is missing).
    const QString defaultScene = app::toQString(project_.defaultScenePath());
    if (!QFileInfo::exists(defaultScene) || !openScene(defaultScene, false)) {
        log::warn("Editor", "default scene '{}' unavailable - starting with an empty scene", defaultScene.toStdString());
        setEditScene(createTemplateScene(ProjectTemplate::Empty), defaultScene);
        ctx_.setDirty(true);
    }
    updatePlayState();
    log::info("Editor", "opened project '{}'", project_.name());
}

EditorWindow::~EditorWindow() {
    // Tear down UI (including the native viewport and its swapchain) before the scenes and
    // context it references.
    for (QDockWidget* dock : findChildren<QDockWidget*>()) delete dock;
    delete takeCentralWidget();
    viewport_ = nullptr;
    simulation_.reset();
}

void EditorWindow::buildActions() {
    using app::theme::icon;
    auto make = [this](const QString& text, const QString& iconName, const QKeySequence& shortcut = {}) {
        auto* a = new QAction(iconName.isEmpty() ? QIcon() : icon(iconName), text, this);
        if (!shortcut.isEmpty()) a->setShortcut(shortcut);
        return a;
    };
    newScene_ = make(tr("New Scene"), QString(), QKeySequence::New);
    openScene_ = make(tr("Open Scene…"), QStringLiteral("open"), QKeySequence::Open);
    saveScene_ = make(tr("Save Scene"), QStringLiteral("save"), QKeySequence::Save);
    saveScene_->setShortcutContext(Qt::ApplicationShortcut);
    saveSceneAs_ = make(tr("Save Scene As…"), QString(), QKeySequence::SaveAs);
    projectSettings_ = make(tr("Project Settings…"), QStringLiteral("settings"));
    closeProject_ = make(tr("Close Project"), QStringLiteral("back"));
    exit_ = make(tr("Exit"), QString(), QKeySequence::Quit);
    duplicate_ = make(tr("Duplicate"), QString(), QKeySequence(Qt::CTRL | Qt::Key_D));
    delete_ = make(tr("Delete"), QStringLiteral("trash"), QKeySequence::Delete);
    frame_ = make(tr("Frame Selection"), QString(), QKeySequence(Qt::Key_F));
    play_ = make(tr("Play"), QStringLiteral("play"), QKeySequence(Qt::Key_F5));
    pause_ = make(tr("Pause"), QStringLiteral("pause"), QKeySequence(Qt::Key_F6));
    stop_ = make(tr("Stop"), QStringLiteral("stop"), QKeySequence(Qt::SHIFT | Qt::Key_F5));
    pilot_ = make(tr("Look Through Scene Camera"), QStringLiteral("camera"), QKeySequence(Qt::Key_F8));
    pilot_->setCheckable(true);
    pilot_->setChecked(true);
    grid_ = make(tr("Show Grid"), QStringLiteral("grid"), QKeySequence(Qt::Key_G));
    grid_->setCheckable(true);
    grid_->setChecked(true);
    vsync_ = make(tr("V-Sync"), QString());
    vsync_->setCheckable(true);
    vsync_->setChecked(app_.options().vsync);
    resetLayout_ = make(tr("Reset Layout"), QString());

    for (QAction* a : {play_, pause_, stop_}) a->setShortcutContext(Qt::ApplicationShortcut);

    connect(newScene_, &QAction::triggered, this, &EditorWindow::newScene);
    connect(openScene_, &QAction::triggered, this, &EditorWindow::openSceneDialog);
    connect(saveScene_, &QAction::triggered, this, &EditorWindow::saveScene);
    connect(saveSceneAs_, &QAction::triggered, this, &EditorWindow::saveSceneAs);
    connect(projectSettings_, &QAction::triggered, this, [this] {
        app::ProjectSettingsDialog dialog(project_, this);
        if (dialog.exec() == QDialog::Accepted) {
            app_.recentProjects().touch(project_.descriptorPath(), project_.name());
            app_.saveRecentProjects();
            updateTitle();
        }
    });
    connect(closeProject_, &QAction::triggered, this, [this] {
        if (!maybeSave()) return;
        stop();
        emit closeProjectRequested();
    });
    connect(exit_, &QAction::triggered, this, &QWidget::close);
    connect(duplicate_, &QAction::triggered, &ctx_, &EditorContext::duplicateSelected);
    connect(delete_, &QAction::triggered, &ctx_, &EditorContext::deleteSelected);
    connect(frame_, &QAction::triggered, this, [this] { client_->frameSelection(); });
    connect(play_, &QAction::triggered, this, &EditorWindow::play);
    connect(pause_, &QAction::triggered, this, &EditorWindow::togglePause);
    connect(stop_, &QAction::triggered, this, &EditorWindow::stop);
    connect(pilot_, &QAction::toggled, this, [this](bool on) { client_->pilotSceneCamera = on; });
    connect(grid_, &QAction::toggled, this, [this](bool on) { client_->showGrid = on; });
    connect(vsync_, &QAction::toggled, this, [this](bool on) {
        if (viewport_) viewport_->setVsync(on);
    });
    connect(resetLayout_, &QAction::triggered, this, &EditorWindow::resetLayout);
}

void EditorWindow::buildCentral() {
    viewportFrame_ = new QFrame;
    viewportFrame_->setObjectName(QStringLiteral("viewportFrame"));
    auto* frameLayout = new QVBoxLayout(viewportFrame_);
    frameLayout->setContentsMargins(2, 2, 2, 2);
    viewportStack_ = new QStackedWidget;
    frameLayout->addWidget(viewportStack_);

    viewport_ = new app::VulkanViewport(app_, *client_);
    QWidget* container = QWidget::createWindowContainer(viewport_);
    container->setFocusPolicy(Qt::StrongFocus);
    container->setMinimumSize(320, 200);
    app::registerViewportContainer(container, viewport_);
    viewportStack_->addWidget(container);

    viewportError_ = new QLabel;
    viewportError_->setAlignment(Qt::AlignCenter);
    viewportError_->setWordWrap(true);
    viewportError_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    viewportError_->setStyleSheet(QStringLiteral("background: #101114; color: #ec6a5e; padding: 40px; font-size: 11pt;"));
    viewportStack_->addWidget(viewportError_);
    setCentralWidget(viewportFrame_);

    connect(viewport_, &app::VulkanViewport::renderFailed, this, &EditorWindow::onRenderFailed);
    if (viewport_->hasFailed()) {
        QTimer::singleShot(0, this, [this] { onRenderFailed(app_.rendererError()); });
    }
}

void EditorWindow::onRenderFailed(const QString& message) {
    viewportError_->setText(tr("Viewport unavailable\n\n%1").arg(message));
    viewportStack_->setCurrentWidget(viewportError_);
}

void EditorWindow::buildDocks() {
    auto* hierarchy = makeDock(this, tr("Hierarchy"), QStringLiteral("dock.hierarchy"), new SceneHierarchyPanel(ctx_));
    auto* inspector = makeDock(this, tr("Inspector"), QStringLiteral("dock.inspector"), new InspectorPanel(ctx_));
    auto* content = new ContentBrowserPanel(app::toQString(project_.contentDir()));
    auto* contentDock = makeDock(this, tr("Content Browser"), QStringLiteral("dock.content"), content);
    auto* console = makeDock(this, tr("Console"), QStringLiteral("dock.console"), new ConsolePanel(app_.logBridge()));
    profiler_ = new ProfilerPanel(app_.renderContext(), [this]() -> const render::FrameStats* {
        render::Renderer* r = app_.renderer();
        return r ? &r->stats() : nullptr;
    });
    auto* profilerDock = makeDock(this, tr("Profiler"), QStringLiteral("dock.profiler"), profiler_);

    addDockWidget(Qt::LeftDockWidgetArea, hierarchy);
    addDockWidget(Qt::RightDockWidgetArea, inspector);
    addDockWidget(Qt::RightDockWidgetArea, profilerDock);
    tabifyDockWidget(inspector, profilerDock);
    inspector->raise();
    addDockWidget(Qt::BottomDockWidgetArea, contentDock);
    addDockWidget(Qt::BottomDockWidgetArea, console);
    tabifyDockWidget(contentDock, console);
    contentDock->raise();
    resizeDocks({hierarchy, inspector}, {280, 360}, Qt::Horizontal);
    resizeDocks({contentDock}, {230}, Qt::Vertical);

    connect(content, &ContentBrowserPanel::openSceneRequested, this, [this](const QString& path) { openScene(path); });
    connect(viewport_, &app::VulkanViewport::frameRendered, this,
            [this] { profiler_->onFrame(viewport_->frameIntervalMs()); });
}

void EditorWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addActions({newScene_, openScene_, saveScene_, saveSceneAs_});
    file->addSeparator();
    file->addAction(projectSettings_);
    file->addSeparator();
    file->addActions({closeProject_, exit_});

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addActions({duplicate_, delete_});
    edit->addSeparator();
    edit->addAction(frame_);

    QMenu* create = menuBar()->addMenu(tr("&Create"));
    populateCreateMenu(*create, ctx_, false);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    for (QDockWidget* dock : findChildren<QDockWidget*>()) view->addAction(dock->toggleViewAction());
    view->addSeparator();
    view->addActions({grid_, vsync_});
    view->addSeparator();
    view->addAction(resetLayout_);

    QMenu* playMenu = menuBar()->addMenu(tr("&Play"));
    playMenu->addActions({play_, pause_, stop_});
    playMenu->addSeparator();
    playMenu->addAction(pilot_);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Viewport Controls"), this, &EditorWindow::showControls);
    help->addAction(tr("About Lumora"), this, [this] {
        QMessageBox::about(this, tr("About Lumora"),
                           tr("<b>%1</b> %2 (%3)<br>Qt %4 · Vulkan 1.4")
                               .arg(QString::fromLatin1(version::kEngineName), QString::fromLatin1(version::kString),
                                    QString::fromLatin1(version::kBuildType), QString::fromLatin1(qVersion())));
    });
}

void EditorWindow::buildToolbar() {
    auto* bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("toolbar.main"));
    bar->setMovable(false);
    bar->setIconSize(QSize(18, 18));
    bar->addAction(saveScene_);
    auto* createButton = new QToolButton;
    createButton->setIcon(app::theme::icon(QStringLiteral("plus")));
    createButton->setToolTip(tr("Create entity"));
    createButton->setPopupMode(QToolButton::InstantPopup);
    auto* createMenu = new QMenu(createButton);
    populateCreateMenu(*createMenu, ctx_, false);
    createButton->setMenu(createMenu);
    bar->addWidget(createButton);
    bar->addSeparator();
    bar->addAction(grid_);
    bar->addAction(pilot_);

    auto* leftSpacer = new QWidget;
    leftSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(leftSpacer);
    bar->addAction(play_);
    bar->addAction(pause_);
    bar->addAction(stop_);
    auto* rightSpacer = new QWidget;
    rightSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(rightSpacer);
    auto* projectLabel = new QLabel(QString::fromStdString(project_.name()));
    projectLabel->setProperty("muted", true);
    projectLabel->setContentsMargins(0, 0, 8, 0);
    bar->addWidget(projectLabel);
}

void EditorWindow::buildStatusBar() {
    statusScene_ = new QLabel;
    statusCamera_ = new QLabel;
    statusFps_ = new QLabel;
    statusGpu_ = new QLabel;
    for (QLabel* l : {statusCamera_, statusFps_, statusGpu_}) {
        l->setContentsMargins(10, 0, 10, 0);
        statusBar()->addPermanentWidget(l);
    }
    statusBar()->addWidget(statusScene_);
    if (render::RenderContext* rc = app_.renderContext()) {
        statusGpu_->setText(QString::fromStdString(rc->caps().name));
    }
    auto* timer = new QTimer(this);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this] {
        statusCamera_->setText(tr("Camera speed %1 m/s").arg(client_->camera().speed(), 0, 'f', 1));
        if (viewport_ && viewport_->frameIntervalMs() > 0.0) {
            statusFps_->setText(tr("%1 fps").arg(1000.0 / viewport_->frameIntervalMs(), 0, 'f', 0));
        }
    });
    timer->start();
}

void EditorWindow::restoreLayout() {
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("editor/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("editor/state")).toByteArray());
}

void EditorWindow::resetLayout() {
    restoreState(defaultLayout_);
    for (QDockWidget* dock : findChildren<QDockWidget*>()) dock->show();
}

// ---- Scenes ---------------------------------------------------------------------------------

void EditorWindow::setEditScene(std::unique_ptr<Scene> scene, const QString& path) {
    stop();
    scene->updateWorldTransforms();
    std::unique_ptr<Scene> old = std::move(editScene_);
    editScene_ = std::move(scene);
    scenePath_ = path;
    ctx_.setScene(editScene_.get(), false);  // panels detach from `old` before it is destroyed
    old.reset();
    ctx_.setDirty(false);

    // Start the editor camera where the scene's camera is, which frames the content nicely.
    if (const entt::entity cam = editScene_->primaryCamera(); cam != entt::null) {
        const auto& t = editScene_->registry().get<TransformComponent>(cam);
        client_->camera().setPosition(glm::vec3(editScene_->worldMatrix(cam)[3]));
        client_->camera().setRotation(t.rotationDeg.y, t.rotationDeg.x);
    }
    updateTitle();
}

bool EditorWindow::openScene(const QString& path, bool prompt) {
    if (prompt && !maybeSave()) return false;
    auto scene = SceneSerializer::load(app::toPath(path));
    if (!scene) {
        QMessageBox::critical(this, tr("Cannot open scene"), QString::fromStdString(scene.error().message));
        return false;
    }
    setEditScene(std::move(*scene), path);
    log::info("Editor", "opened scene {}", path.toStdString());
    return true;
}

void EditorWindow::newScene() {
    if (!maybeSave()) return;
    setEditScene(createTemplateScene(ProjectTemplate::Empty), QString());
    editScene_->setName("Untitled");
    updateTitle();
}

void EditorWindow::openSceneDialog() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Scene"), app::toQString(project_.scenesDir()),
                                                      tr("Lumora scene (*.lscene)"));
    if (!path.isEmpty()) openScene(path);
}

bool EditorWindow::saveScene() {
    if (scenePath_.isEmpty()) return saveSceneAs();
    if (auto saved = SceneSerializer::save(*editScene_, app::toPath(scenePath_)); !saved) {
        QMessageBox::critical(this, tr("Save failed"), QString::fromStdString(saved.error().message));
        return false;
    }
    if (auto saved = project_.save(); !saved) {
        log::warn("Editor", "project descriptor not saved: {}", saved.error().message);
    }
    saveThumbnail();
    ctx_.setDirty(false);
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(scenePath_).fileName()), 4000);
    log::info("Editor", "saved scene {}", scenePath_.toStdString());
    return true;
}

bool EditorWindow::saveSceneAs() {
    QString path = QFileDialog::getSaveFileName(this, tr("Save Scene As"), app::toQString(project_.scenesDir()),
                                                tr("Lumora scene (*.lscene)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(QLatin1String(".lscene"))) path += QStringLiteral(".lscene");
    scenePath_ = path;
    editScene_->setName(QFileInfo(path).completeBaseName().toStdString());
    return saveScene();
}

bool EditorWindow::maybeSave() {
    if (!ctx_.isDirty()) return true;
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved changes"), tr("Save changes to '%1'?").arg(QString::fromStdString(editScene_->name())),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return saveScene();
    return true;
}

void EditorWindow::saveThumbnail() {
    render::Renderer* renderer = app_.renderer();
    if (renderer == nullptr || !editScene_) return;
    try {
        editScene_->updateWorldTransforms();
        auto image = renderer->capture(*editScene_, client_->camera().view(16.0f / 9.0f), {640, 360}, {});
        if (!image) {
            log::warn("Editor", "thumbnail capture failed: {}", image.error().message);
            return;
        }
        const QImage qimg(image->rgba.data(), static_cast<int>(image->width), static_cast<int>(image->height),
                          QImage::Format_RGBA8888);
        const QString path = app::toQString(project_.thumbnailPath());
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!qimg.save(path, "PNG")) log::warn("Editor", "could not write {}", path.toStdString());
    } catch (const std::exception& e) {
        log::error("Editor", "thumbnail capture failed: {}", e.what());
    }
}

void EditorWindow::updateTitle() {
    const QString sceneName = editScene_ ? QString::fromStdString(editScene_->name()) : tr("No scene");
    setWindowTitle(QStringLiteral("%1%2 — %3 — Lumora Editor")
                       .arg(sceneName, ctx_.isDirty() ? QStringLiteral("*") : QString(),
                            QString::fromStdString(project_.name())));
    statusScene_->setText(scenePath_.isEmpty() ? tr("Unsaved scene")
                                               : app::toQString(project_.relativePath(app::toPath(scenePath_))));
}

// ---- Play mode ------------------------------------------------------------------------------

void EditorWindow::play() {
    if (simulation_) {
        paused_ = false;
        updatePlayState();
        return;
    }
    auto copy = SceneSerializer::clone(*editScene_);
    if (!copy) {
        QMessageBox::critical(this, tr("Cannot start play mode"), QString::fromStdString(copy.error().message));
        return;
    }
    playScene_ = std::move(*copy);
    simulation_ = std::make_unique<Simulation>(*playScene_);
    paused_ = false;
    ctx_.setScene(playScene_.get(), true);
    updatePlayState();
    log::info("Editor", "play mode started");
}

void EditorWindow::togglePause() {
    if (!simulation_) return;
    paused_ = !paused_;
    updatePlayState();
}

void EditorWindow::stop() {
    if (!simulation_) return;
    log::info("Editor", "play mode stopped after {:.2f} s simulated", simulation_->simulatedSeconds());
    simulation_.reset();
    ctx_.setScene(editScene_.get(), false);
    playScene_.reset();
    paused_ = false;
    updatePlayState();
}

void EditorWindow::updatePlayState() {
    const bool playing = simulation_ != nullptr;
    play_->setEnabled(!playing || paused_);
    pause_->setEnabled(playing);
    pause_->setText(paused_ ? tr("Resume") : tr("Pause"));
    stop_->setEnabled(playing);
    const char* border = !playing ? "#121316" : (paused_ ? "#e8b84a" : "#5fcf8a");
    viewportFrame_->setStyleSheet(QStringLiteral("#viewportFrame { background: %1; }").arg(QString::fromLatin1(border)));
    for (QAction* a : {newScene_, openScene_, saveSceneAs_}) a->setEnabled(!playing);
}

bool EditorWindow::handleShortcut(int key, Qt::KeyboardModifiers mods) {
    // Shortcuts for keys typed while the native viewport window has focus.
    if (QGuiApplication::mouseButtons() & Qt::RightButton) return false;  // flying
    const bool ctrl = mods & Qt::ControlModifier;
    if (key == Qt::Key_Delete) {
        ctx_.deleteSelected();
    } else if (key == Qt::Key_D && ctrl) {
        ctx_.duplicateSelected();
    } else if (key == Qt::Key_F && !ctrl) {
        client_->frameSelection();
    } else if (key == Qt::Key_G && !ctrl) {
        grid_->toggle();
    } else if (key == Qt::Key_Escape) {
        ctx_.clearSelection();
    } else {
        return false;
    }
    return true;
}

void EditorWindow::showControls() {
    QMessageBox::information(this, tr("Viewport Controls"),
                             tr("<table cellspacing='6'>"
                                "<tr><td><b>Right mouse + drag</b></td><td>Look around</td></tr>"
                                "<tr><td><b>Right mouse + W A S D</b></td><td>Fly</td></tr>"
                                "<tr><td><b>Right mouse + Q / E</b></td><td>Down / up</td></tr>"
                                "<tr><td><b>Shift</b></td><td>Move faster</td></tr>"
                                "<tr><td><b>Right mouse + wheel</b></td><td>Change fly speed</td></tr>"
                                "<tr><td><b>Middle mouse + drag</b></td><td>Pan</td></tr>"
                                "<tr><td><b>Wheel</b></td><td>Dolly</td></tr>"
                                "<tr><td><b>Left click</b></td><td>Select</td></tr>"
                                "<tr><td><b>F</b></td><td>Frame selection</td></tr>"
                                "<tr><td><b>G</b></td><td>Toggle grid</td></tr>"
                                "<tr><td><b>F5 / F6 / Shift+F5</b></td><td>Play / Pause / Stop</td></tr>"
                                "<tr><td><b>F8</b></td><td>Toggle scene camera in play mode</td></tr>"
                                "</table>"));
}

void EditorWindow::closeEvent(QCloseEvent* event) {
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    stop();
    QSettings settings;
    settings.setValue(QStringLiteral("editor/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("editor/state"), saveState());
    event->accept();
}

} // namespace lumora::editor
