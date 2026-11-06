#include "editor/EditorWindow.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/GameExporter.hpp"
#include "common/ScriptBuilder.hpp"
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
#include "runtime/AudioWorld.hpp"
#include "runtime/Scripting.hpp"
#include "runtime/Simulation.hpp"
#include "assets/AssetImport.hpp"
#include "assets/Image.hpp"
#include "collab/CollabClient.hpp"
#include "collab/CollabServer.hpp"
#include "collab/HostIdentity.hpp"
#include "collab/CollabSession.hpp"
#include "collab/CollabUi.hpp"
#include "renderer/MeshCache.hpp"
#include "renderer/TextureCache.hpp"
#include "scene/ScenePatch.hpp"
#include "scene/ModelInstance.hpp"
#include "scene/PhysicsHelpers.hpp"
#include "scene/Prefab.hpp"
#include "scene/SceneSerializer.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {

/// Compact dock title bar: panel icon + title, float/close buttons on the right.
QWidget* makeDockTitleBar(QDockWidget* dock, const QString& iconName) {
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("dockTitle"));
    bar->setAttribute(Qt::WA_StyledBackground);
    bar->setStyleSheet(QStringLiteral(
        "#dockTitle { background: #16181d; border-bottom: 1px solid #23262e; }"
        "#dockTitle QLabel#dockTitleText { color: #d3d7de; font-weight: 600; }"
        "#dockTitle QToolButton { border: none; padding: 2px; border-radius: 4px; }"
        "#dockTitle QToolButton:hover { background: #262a32; }"));
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 5, 6, 5);
    layout->setSpacing(7);
    auto* icon = new QLabel;
    icon->setPixmap(app::theme::icon(iconName).pixmap(QSize(15, 15), bar->devicePixelRatioF()));
    auto* title = new QLabel(dock->windowTitle());
    title->setObjectName(QStringLiteral("dockTitleText"));
    layout->addWidget(icon);
    layout->addWidget(title);
    layout->addStretch();
    for (const auto& [iconId, tip, isClose] : {std::tuple{"open", "Float / dock", false}, std::tuple{"close", "Close panel", true}}) {
        auto* button = new QToolButton;
        button->setIcon(app::theme::icon(QString::fromLatin1(iconId)));
        button->setIconSize(QSize(12, 12));
        button->setToolTip(QObject::tr(tip));
        layout->addWidget(button);
        if (isClose) {
            QObject::connect(button, &QToolButton::clicked, dock, &QDockWidget::close);
        } else {
            QObject::connect(button, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
        }
    }
    return bar;
}

QDockWidget* makeDock(QMainWindow* window, const QString& title, const QString& objectName, const QString& iconName,
                      QWidget* content) {
    auto* dock = new QDockWidget(title, window);
    dock->setObjectName(objectName);
    dock->setWidget(content);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                      QDockWidget::DockWidgetClosable);
    dock->setTitleBarWidget(makeDockTitleBar(dock, iconName));
    return dock;
}

} // namespace

EditorWindow::EditorWindow(app::AppContext& app, Project project, QWidget* parent)
    : QMainWindow(parent), app_(app), project_(std::move(project)), assets_(project_.rootDir()),
      client_(std::make_unique<EditorViewportClient>(ctx_)) {
    client_->assets = &assets_;
    setWindowIcon(app::theme::icon(QStringLiteral("logo")));
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AnimatedDocks);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    resize(1600, 960);

    client_->onSimulate = [this](float dt) {
        if (simulation_ && !paused_) simulation_->advance(dt);
        if (simulation_) {
            // The HUD is laid out in the viewport's pixels.
            const qreal dpr = viewport_->devicePixelRatio();
            simulation_->hud().screenSize = glm::vec2(viewport_->width() * dpr, viewport_->height() * dpr);
        }
        if (simulation_ && simulation_->audio() != nullptr) {
            simulation_->audio()->setListenerFromView(client_->viewportCamera(1.0f).view);  // hear from the camera
        }
    };
    client_->onShortcut = [this](int key, Qt::KeyboardModifiers mods) { return handleShortcut(key, mods); };
    ctx_.onComponentAdded = [this](Scene& scene, entt::entity e, const ComponentDesc& component) {
        // A new collider starts out matching the entity's mesh.
        if (component.id != "Collider") return;
        const auto* mesh = scene.registry().try_get<MeshRendererComponent>(e);
        if (mesh == nullptr) return;
        const Aabb* bounds = nullptr;
        if (!mesh->meshAsset.empty()) {
            if (auto asset = assets_.mesh(mesh->meshAsset)) bounds = &(*asset)->data.bounds;
        }
        fitColliderToMesh(scene.registry().get<ColliderComponent>(e), *mesh, bounds);
    };

    buildActions();
    buildCentral();
    buildDocks();
    buildMenus();
    buildToolbar();
    buildStatusBar();
    defaultLayout_ = saveState();
    restoreLayout();
    setUpScripts();

    connect(&ctx_, &EditorContext::dirtyChanged, this, &EditorWindow::updateTitle);

    // Load the project's default scene (or start a fresh one if it is missing).
    const QString defaultScene = app::toQString(project_.defaultScenePath());
    if (!QFileInfo::exists(defaultScene) || !openScene(defaultScene, false)) {
        log::warn("Editor", "default scene '{}' unavailable - starting with an empty scene", defaultScene.toStdString());
        setEditScene(createTemplateScene(ProjectTemplate::Empty), defaultScene);
        ctx_.setDirty(true);
    }
    updatePlayState();
    updateSessionUi();
    log::info("Editor", "opened project '{}'", project_.name());
}

EditorWindow::~EditorWindow() {
    // End any live session first: it references the scene, the context and the panel.
    collabSession_.reset();
    collabClient_.reset();
    collabServer_.reset();
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
    importModel_ = make(tr("Import Model…"), QStringLiteral("cube"), QKeySequence(Qt::CTRL | Qt::Key_I));
    connect(importModel_, &QAction::triggered, this, &EditorWindow::importModelDialog);
    projectSettings_ = make(tr("Project Settings…"), QStringLiteral("settings"));
    exportGame_ = make(tr("Export Game…"), QStringLiteral("save"));
    connect(exportGame_, &QAction::triggered, this, &EditorWindow::exportGameDialog);
    hostSession_ = make(tr("Host Session…"), QStringLiteral("users"));
    joinSession_ = make(tr("Join Session…"), QString());
    leaveSession_ = make(tr("Leave Session"), QString());
    connect(hostSession_, &QAction::triggered, this, &EditorWindow::hostSession);
    connect(joinSession_, &QAction::triggered, this, [this] {
        if (prepareToClose()) emit joinSessionRequested();
    });
    connect(leaveSession_, &QAction::triggered, this, [this] {
        if (collabServer_ && collabServer_->clientCount() > 1 &&
            QMessageBox::question(this, tr("End Session"),
                                  tr("You are hosting. Leaving ends the session for everyone. Continue?")) !=
                QMessageBox::Yes) {
            return;
        }
        leaveSession(tr("You left the session."));
    });
    closeProject_ = make(tr("Close Project"), QStringLiteral("back"));
    exit_ = make(tr("Exit"), QString(), QKeySequence::Quit);
    undo_ = ctx_.undoStack().createUndoAction(this, tr("Undo"));
    undo_->setShortcut(QKeySequence::Undo);
    undo_->setIcon(icon(QStringLiteral("undo")));
    redo_ = ctx_.undoStack().createRedoAction(this, tr("Redo"));
    redo_->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    redo_->setIcon(icon(QStringLiteral("redo")));
    duplicate_ = make(tr("Duplicate"), QString(), QKeySequence(Qt::CTRL | Qt::Key_D));
    delete_ = make(tr("Delete"), QStringLiteral("trash"), QKeySequence::Delete);
    frame_ = make(tr("Frame Selection"), QString(), QKeySequence(Qt::Key_F));
    savePrefab_ = make(tr("Save Selection as Prefab…"), QStringLiteral("save"));
    connect(savePrefab_, &QAction::triggered, this, &EditorWindow::saveSelectionAsPrefab);
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
    vsync_->setChecked(app_.options().vsync.value_or(true));
    resetLayout_ = make(tr("Reset Layout"), QString());
    newBehaviour_ = make(tr("New Behaviour…"), QStringLiteral("plus"));
    buildScripts_ = make(tr("Build Scripts"), QString(), QKeySequence(Qt::CTRL | Qt::Key_B));
    openScripts_ = make(tr("Open Scripts Folder"), QStringLiteral("folder"));
    connect(newBehaviour_, &QAction::triggered, this, &EditorWindow::newBehaviour);
    connect(buildScripts_, &QAction::triggered, this, [this] {
        if (scripts_) scripts_->build();
    });
    connect(openScripts_, &QAction::triggered, this, [this] {
        QDir().mkpath(scripts_->scriptsDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(scripts_->scriptsDir()));
    });
    // Gizmo tools. 1/2/3 are handled while the viewport has the keyboard (handleShortcut), so
    // they never collide with typing; WASDQE fly the camera.
    auto* tools = new QActionGroup(this);
    moveTool_ = make(tr("Move (1)"), QStringLiteral("move"));
    rotateTool_ = make(tr("Rotate (2)"), QStringLiteral("rotate"));
    scaleTool_ = make(tr("Scale (3)"), QStringLiteral("scale"));
    for (QAction* a : {moveTool_, rotateTool_, scaleTool_}) {
        a->setCheckable(true);
        tools->addAction(a);
    }
    moveTool_->setChecked(true);
    connect(moveTool_, &QAction::triggered, this, [this] { client_->gizmo().setMode(GizmoMode::Translate); });
    connect(rotateTool_, &QAction::triggered, this, [this] { client_->gizmo().setMode(GizmoMode::Rotate); });
    connect(scaleTool_, &QAction::triggered, this, [this] { client_->gizmo().setMode(GizmoMode::Scale); });

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
        saveLayout();
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
    viewport_->setInputContainer(container);
    viewportStack_->addWidget(container);
    // Clicking the viewport takes focus away from text fields, so keys fly the camera; the
    // container also starts with focus instead of the first line edit in the tab chain.
    connect(viewport_, &app::VulkanViewport::pointerPressed, container,
            [container] { container->setFocus(Qt::MouseFocusReason); });

    viewportError_ = new QLabel;
    viewportError_->setAlignment(Qt::AlignCenter);
    viewportError_->setWordWrap(true);
    viewportError_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    viewportError_->setStyleSheet(QStringLiteral("background: #101114; color: #ec6a5e; padding: 40px; font-size: 11pt;"));
    viewportStack_->addWidget(viewportError_);
    setCentralWidget(viewportFrame_);
    container->setFocus();  // now inside this window, so it becomes the initial focus widget

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
    auto* hierarchy = makeDock(this, tr("Hierarchy"), QStringLiteral("dock.hierarchy"), QStringLiteral("hierarchy"), new SceneHierarchyPanel(ctx_));
    auto* inspector = makeDock(this, tr("Inspector"), QStringLiteral("dock.inspector"), QStringLiteral("settings"), new InspectorPanel(ctx_, [this](QWidget* parent, const FieldDesc& field) {
                     if (field.id == "skyHdr") return pickHdrAsset(parent);
                     if (field.id == "audioClip") return pickAudioAsset(parent);
                     return field.id.ends_with("Texture") ? pickTextureAsset(parent) : pickMeshAsset(parent);
                 }));
    auto* content = new ContentBrowserPanel(app::toQString(project_.contentDir()));
    auto* contentDock = makeDock(this, tr("Content Browser"), QStringLiteral("dock.content"), QStringLiteral("folder"), content);
    auto* console = makeDock(this, tr("Console"), QStringLiteral("dock.console"), QStringLiteral("console"), new ConsolePanel(app_.logBridge()));
    profiler_ = new ProfilerPanel(app_.renderContext(), [this]() -> const render::FrameStats* {
        render::Renderer* r = app_.renderer();
        return r ? &r->stats() : nullptr;
    });
    auto* profilerDock = makeDock(this, tr("Profiler"), QStringLiteral("dock.profiler"), QStringLiteral("chart"), profiler_);

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

    collabPanel_ = new collab::CollabPanel;
    collabPanel_->entityName = [this](const QString& uuid) -> QString {
        const auto id = Uuid::parse(uuid.toStdString());
        const entt::entity e = id && editScene_ ? editScene_->findByUuid(*id) : entt::entity{entt::null};
        return e == entt::null ? QString() : QString::fromStdString(editScene_->nameOf(e));
    };
    collabDock_ = makeDock(this, tr("Session"), QStringLiteral("dock.session"), QStringLiteral("users"), collabPanel_);
    addDockWidget(Qt::RightDockWidgetArea, collabDock_);
    tabifyDockWidget(inspector, collabDock_);
    inspector->raise();
    connect(collabPanel_, &collab::CollabPanel::hostRequested, this, &EditorWindow::hostSession);
    connect(collabPanel_, &collab::CollabPanel::joinRequested, joinSession_, &QAction::trigger);
    connect(collabPanel_, &collab::CollabPanel::leaveRequested, leaveSession_, &QAction::trigger);

    connect(content, &ContentBrowserPanel::openSceneRequested, this, [this](const QString& path) { openScene(path); });
    connect(content, &ContentBrowserPanel::addModelRequested, this, [this](const QString& path) { addModelToScene(path); });
    connect(content, &ContentBrowserPanel::importRequested, this, &EditorWindow::importModels);
    connect(viewport_, &app::VulkanViewport::frameRendered, this,
            [this] { profiler_->onFrame(viewport_->frameIntervalMs()); });
}

void EditorWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addActions({newScene_, openScene_, saveScene_, saveSceneAs_});
    file->addSeparator();
    file->addAction(importModel_);
    file->addSeparator();
    file->addAction(projectSettings_);
    file->addAction(exportGame_);
    file->addSeparator();
    file->addActions({closeProject_, exit_});

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addActions({undo_, redo_});
    edit->addSeparator();
    edit->addActions({duplicate_, delete_});
    edit->addSeparator();
    edit->addAction(frame_);
    edit->addSeparator();
    edit->addAction(savePrefab_);

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

    QMenu* scriptsMenu = menuBar()->addMenu(tr("&Scripts"));
    scriptsMenu->addActions({newBehaviour_, buildScripts_});
    scriptsMenu->addSeparator();
    scriptsMenu->addAction(openScripts_);

    QMenu* collaborate = menuBar()->addMenu(tr("&Collaborate"));
    collaborate->addActions({hostSession_, joinSession_});
    collaborate->addSeparator();
    collaborate->addAction(leaveSession_);
    collaborate->addSeparator();
    collaborate->addAction(collabDock_->toggleViewAction());

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
    bar->setStyleSheet(QStringLiteral("QToolBar { padding: 5px 10px; spacing: 6px; }"));

    // Left: brand + breadcrumb (project › scene).
    auto* logo = new QLabel;
    logo->setPixmap(app::theme::icon(QStringLiteral("logo")).pixmap(QSize(22, 22), devicePixelRatioF()));
    logo->setContentsMargins(2, 0, 6, 0);
    bar->addWidget(logo);
    auto* projectLabel = new QLabel(QString::fromStdString(project_.name()));
    projectLabel->setStyleSheet(QStringLiteral("font-weight: 650; color: #eef0f3;"));
    bar->addWidget(projectLabel);
    auto* chevron = new QLabel(QStringLiteral("›"));
    chevron->setStyleSheet(QStringLiteral("color: #5d6472; font-size: 13pt; padding: 0 4px;"));
    bar->addWidget(chevron);
    toolbarScene_ = new QLabel;
    toolbarScene_->setStyleSheet(QStringLiteral("color: #9aa1ad;"));
    bar->addWidget(toolbarScene_);
    bar->addSeparator();

    bar->addAction(saveScene_);
    bar->addAction(undo_);
    bar->addAction(redo_);
    auto* createButton = new QToolButton;
    createButton->setIcon(app::theme::icon(QStringLiteral("plus")));
    createButton->setToolTip(tr("Create entity"));
    createButton->setPopupMode(QToolButton::InstantPopup);
    auto* createMenu = new QMenu(createButton);
    populateCreateMenu(*createMenu, ctx_, false);
    createButton->setMenu(createMenu);
    bar->addWidget(createButton);
    bar->addSeparator();
    bar->addAction(moveTool_);
    bar->addAction(rotateTool_);
    bar->addAction(scaleTool_);
    bar->addSeparator();
    bar->addAction(grid_);
    bar->addAction(pilot_);

    // Centre: play controls in a pill, plus a mode chip.
    auto* leftSpacer = new QWidget;
    leftSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(leftSpacer);
    auto* pill = new QFrame;
    pill->setObjectName(QStringLiteral("playPill"));
    pill->setStyleSheet(QStringLiteral("#playPill { background: #1c1f26; border: 1px solid #2a2e37; border-radius: 9px; }"
                                       "#playPill QToolButton { border: none; border-radius: 6px; padding: 5px 9px; }"
                                       "#playPill QToolButton:hover { background: #2a2f39; }"
                                       "#playPill QToolButton:disabled { background: transparent; }"));
    auto* pillLayout = new QHBoxLayout(pill);
    pillLayout->setContentsMargins(3, 3, 3, 3);
    pillLayout->setSpacing(2);
    for (QAction* a : {play_, pause_, stop_}) {
        auto* b = new QToolButton;
        b->setDefaultAction(a);
        b->setIconSize(QSize(16, 16));
        pillLayout->addWidget(b);
    }
    bar->addWidget(pill);
    modeChip_ = new QLabel;
    modeChip_->setContentsMargins(4, 0, 0, 0);
    bar->addWidget(modeChip_);
    auto* rightSpacer = new QWidget;
    rightSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(rightSpacer);

    bar->addAction(projectSettings_);
}

void EditorWindow::buildStatusBar() {
    statusScene_ = new QLabel;
    statusCamera_ = new QLabel;
    statusFps_ = new QLabel;
    statusGpu_ = new QLabel;
    for (QLabel* l : {statusCamera_, statusFps_, statusGpu_}) {
        l->setStyleSheet(QStringLiteral("background: #1b1e24; border: 1px solid #262a32; border-radius: 4px;"
                                        "padding: 1px 8px; color: #9aa1ad; margin: 2px 3px;"));
        statusBar()->addPermanentWidget(l);
    }
    statusCollab_ = new QLabel;
    statusCollab_->setStyleSheet(QStringLiteral("background: #16301f; border: 1px solid #2b5a3a; border-radius: 4px;"
                                                "padding: 1px 8px; color: #7be0a0; margin: 2px 3px; font-weight: 600;"));
    statusCollab_->hide();
    statusBar()->insertPermanentWidget(0, statusCollab_);
    statusScene_->setContentsMargins(6, 0, 0, 0);
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

void EditorWindow::saveLayout() {
    QSettings settings;
    settings.setValue(QStringLiteral("editor/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("editor/state"), saveState());
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
    ctx_.resetHistory();

    // Start the editor camera where the scene's camera is, which frames the content nicely.
    if (const entt::entity cam = editScene_->primaryCamera(); cam != entt::null) {
        const auto& t = editScene_->registry().get<TransformComponent>(cam);
        client_->camera().setPosition(glm::vec3(editScene_->worldMatrix(cam)[3]));
        client_->camera().setRotation(t.rotationDeg.y, t.rotationDeg.x);
    }
    updateTitle();
}

bool EditorWindow::openScene(const QString& path, bool prompt) {
    if (inSession()) {
        QMessageBox::information(this, tr("Live Session"), tr("Leave the live session to open another scene."));
        return false;
    }
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
    if (inSession() || !maybeSave()) return;
    setEditScene(createTemplateScene(ProjectTemplate::Empty), QString());
    editScene_->setName("Untitled");
    updateTitle();
}

void EditorWindow::importModelDialog() {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Model"), QString(),
                                                            tr("glTF 2.0 models (*.glb *.gltf)"));
    if (!files.isEmpty()) importModels(files, app::toQString(project_.contentDir() / "Models"));
}

void EditorWindow::importModels(const QStringList& files, const QString& targetDir) {
    for (const QString& file : files) {
        if (isPrefabFile(app::toPath(file))) {
            auto copied = importFile(app::toPath(file), project_.contentDir() / "Prefabs");
            if (!copied) {
                QMessageBox::critical(this, tr("Import failed"), QString::fromStdString(copied.error().message));
                continue;
            }
            addPrefabToScene(*copied);
            continue;
        }
        if (isHdrFile(app::toPath(file))) {
            // An HDR environment becomes the scene's sky (undoable like any environment edit).
            auto copied = importFile(app::toPath(file), project_.contentDir() / "Textures");
            if (!copied) {
                QMessageBox::critical(this, tr("Import failed"), QString::fromStdString(copied.error().message));
                continue;
            }
            const std::string key = project_.relativePath(*copied);
            ctx_.setEnvironment(tr("Set HDR Sky"), [&key](EnvironmentSettings& env) { env.skyTexture = key; });
            log::info("Editor", "sky set to {}", key);
            continue;
        }
        auto imported = importModelFile(app::toPath(file), app::toPath(targetDir));
        if (!imported) {
            QMessageBox::critical(this, tr("Import failed"), QString::fromStdString(imported.error().message));
            continue;
        }
        log::info("Editor", "imported {} -> {}", file.toStdString(), imported->string());
        addModelToScene(app::toQString(*imported));
    }
}

entt::entity EditorWindow::addModelToScene(const QString& absolutePath) {
    const std::filesystem::path path = app::toPath(absolutePath);
    if (isPrefabFile(path)) return addPrefabToScene(path);
    const std::string relative = project_.relativePath(path);
    if (relative.empty() || relative.starts_with("..")) {
        QMessageBox::warning(this, tr("Add model"), tr("Only models inside the project can be placed. Use Import Model."));
        return entt::null;
    }
    auto model = assets_.model(std::filesystem::absolute(path));
    if (!model) {
        QMessageBox::critical(this, tr("Cannot load model"), QString::fromStdString(model.error().message));
        return entt::null;
    }
    // Drop it where the camera looks: on the ground plane if the view ray hits it nearby.
    const FlyCameraController& cam = client_->camera();
    glm::vec3 spot = cam.position() + cam.forward() * 5.0f;
    if (cam.forward().y < -0.05f) {
        const float t = -cam.position().y / cam.forward().y;
        if (t > 0.0f && t < 60.0f) spot = cam.position() + cam.forward() * t;
    }
    entt::entity root = entt::null;
    const std::string name = path.stem().string();
    ctx_.edit(tr("Add %1").arg(QString::fromStdString(name)), [&](Scene& scene) {
        root = instantiateModel(scene, **model, relative, name);
        scene.registry().get<TransformComponent>(root).position = spot;
    });
    if (root != entt::null) {
        ctx_.select(editScene_->uuidOf(root));
        statusBar()->showMessage(tr("Added %1 (%2 meshes)").arg(QString::fromStdString(name)).arg((*model)->meshes.size()), 4000);
    }
    return root;
}

std::optional<std::string> EditorWindow::pickMeshAsset(QWidget* parent) {
    QString file = QFileDialog::getOpenFileName(parent, tr("Choose Mesh"), app::toQString(project_.contentDir()),
                                                tr("glTF 2.0 models (*.glb *.gltf)"));
    if (file.isEmpty()) return std::nullopt;
    std::filesystem::path path = std::filesystem::absolute(app::toPath(file));
    std::string relative = project_.relativePath(path);
    if (relative.starts_with("..")) {
        // Outside the project: bring it in first so the reference stays portable.
        auto imported = importModelFile(path, project_.contentDir() / "Models");
        if (!imported) {
            QMessageBox::critical(parent, tr("Import failed"), QString::fromStdString(imported.error().message));
            return std::nullopt;
        }
        path = *imported;
        relative = project_.relativePath(path);
    }
    auto model = assets_.model(path);
    if (!model || (*model)->meshes.empty()) {
        QMessageBox::critical(parent, tr("Cannot use model"),
                              model ? tr("The file contains no meshes.") : QString::fromStdString(model.error().message));
        return std::nullopt;
    }
    int index = 0;
    if ((*model)->meshes.size() > 1) {
        QStringList names;
        for (const ImportedMesh& m : (*model)->meshes) names << QString::fromStdString(m.name);
        bool ok = false;
        const QString chosen = QInputDialog::getItem(parent, tr("Choose Mesh"), tr("Mesh"), names, 0, false, &ok);
        if (!ok) return std::nullopt;
        index = static_cast<int>(names.indexOf(chosen));
    }
    return AssetLibrary::meshKey(relative, index);
}

std::optional<std::string> EditorWindow::pickAudioAsset(QWidget* parent) {
    const QString file = QFileDialog::getOpenFileName(parent, tr("Choose Sound"), app::toQString(project_.contentDir()),
                                                      tr("Sounds (*.wav *.flac *.mp3)"));
    if (file.isEmpty()) return std::nullopt;
    std::filesystem::path path = std::filesystem::absolute(app::toPath(file));
    if (project_.relativePath(path).starts_with("..")) {
        auto imported = importFile(path, project_.contentDir() / "Audio");
        if (!imported) {
            QMessageBox::critical(parent, tr("Import failed"), QString::fromStdString(imported.error().message));
            return std::nullopt;
        }
        path = *imported;
    }
    return project_.relativePath(path);
}

std::optional<std::string> EditorWindow::pickHdrAsset(QWidget* parent) {
    const QString file = QFileDialog::getOpenFileName(parent, tr("Choose HDR Environment"),
                                                      app::toQString(project_.contentDir()),
                                                      tr("Radiance HDR, equirectangular (*.hdr)"));
    if (file.isEmpty()) return std::nullopt;
    std::filesystem::path path = std::filesystem::absolute(app::toPath(file));
    if (project_.relativePath(path).starts_with("..")) {
        auto imported = importFile(path, project_.contentDir() / "Textures");
        if (!imported) {
            QMessageBox::critical(parent, tr("Import failed"), QString::fromStdString(imported.error().message));
            return std::nullopt;
        }
        path = *imported;
    }
    if (auto hdr = loadHdrFile(path); !hdr) {
        QMessageBox::critical(parent, tr("Cannot use image"), QString::fromStdString(hdr.error().message));
        return std::nullopt;
    }
    return project_.relativePath(path);
}

std::optional<std::string> EditorWindow::pickTextureAsset(QWidget* parent) {
    const QString file = QFileDialog::getOpenFileName(
        parent, tr("Choose Texture"), app::toQString(project_.contentDir()),
        tr("Images (*.png *.jpg *.jpeg *.tga *.bmp);;Images inside a glTF model (*.glb *.gltf)"));
    if (file.isEmpty()) return std::nullopt;
    std::filesystem::path path = std::filesystem::absolute(app::toPath(file));
    const bool model = isModelFile(path);
    if (project_.relativePath(path).starts_with("..")) {
        auto imported = model ? importModelFile(path, project_.contentDir() / "Models")
                              : importFile(path, project_.contentDir() / "Textures");
        if (!imported) {
            QMessageBox::critical(parent, tr("Import failed"), QString::fromStdString(imported.error().message));
            return std::nullopt;
        }
        path = *imported;
    }
    const std::string relative = project_.relativePath(path);
    if (!model) {
        auto image = assets_.texture(relative);
        if (!image) {
            QMessageBox::critical(parent, tr("Cannot use image"), QString::fromStdString(image.error().message));
            return std::nullopt;
        }
        return relative;
    }
    auto loaded = assets_.model(path);
    if (!loaded || (*loaded)->images.empty()) {
        QMessageBox::critical(parent, tr("Cannot use model"),
                              loaded ? tr("The model contains no images.") : QString::fromStdString(loaded.error().message));
        return std::nullopt;
    }
    QStringList names;
    for (const ImportedImage& image : (*loaded)->images) names << QString::fromStdString(image.name);
    bool ok = false;
    const QString chosen = QInputDialog::getItem(parent, tr("Choose Texture"), tr("Image"), names, 0, false, &ok);
    if (!ok) return std::nullopt;
    return AssetLibrary::textureKey(relative, static_cast<int>(names.indexOf(chosen)));
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
    ctx_.markSaved();
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
        render::RenderSettings settings;
        settings.assets = &assets_;
        settings.blockOnAssets = true;  // thumbnails must show every model
        auto image = renderer->capture(*editScene_, client_->camera().view(16.0f / 9.0f), {640, 360}, settings);
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
    if (toolbarScene_ != nullptr) {
        toolbarScene_->setText(sceneName + (ctx_.isDirty() ? QStringLiteral("  •") : QString()));
    }
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
    simulation_->setProjectRoot(project_.rootDir());
    if (AudioWorld::sceneHasAudio(*playScene_)) {
        auto audio = AudioWorld::create(*playScene_, {project_.rootDir(), true});
        if (audio) {
            simulation_->setAudio(std::move(*audio));
        } else {
            log::warn("Editor", "{} - playing without sound", audio.error().message);
        }
    }
    if (gameModule_) simulation_->setScripts(gameModule_, &viewport_->input());
    client_->hud = &simulation_->hud();
    paused_ = false;
    ctx_.setScene(playScene_.get(), true);
    updatePlayState();
    log::info("Editor", "play mode started");
}

void EditorWindow::togglePause() {
    if (!simulation_) return;
    paused_ = !paused_;
    if (simulation_->audio() != nullptr) simulation_->audio()->setPaused(paused_);
    updatePlayState();
}

void EditorWindow::stop() {
    if (!simulation_) return;
    log::info("Editor", "play mode stopped after {:.2f} s simulated", simulation_->simulatedSeconds());
    client_->hud = nullptr;
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
    const char* chipText = !playing ? "EDITING" : (paused_ ? "PAUSED" : "PLAYING");
    const char* chipColor = !playing ? "#7c8494" : (paused_ ? "#e8b84a" : "#5fcf8a");
    modeChip_->setText(QString::fromLatin1(chipText));
    modeChip_->setStyleSheet(QStringLiteral("color: %1; font-weight: 700; font-size: 8pt; letter-spacing: 1.2px;"
                                            "border: 1px solid %1; border-radius: 9px; padding: 2px 9px;")
                                 .arg(QString::fromLatin1(chipColor)));
    viewportFrame_->setStyleSheet(QStringLiteral("#viewportFrame { background: %1; }").arg(QString::fromLatin1(border)));
    for (QAction* a : {newScene_, openScene_, saveSceneAs_}) a->setEnabled(!playing);
    ctx_.undoStack().setActive(!playing);
    undo_->setVisible(true);
    if (playing) {
        undo_->setEnabled(false);
        redo_->setEnabled(false);
    } else {
        undo_->setEnabled(ctx_.undoStack().canUndo());
        redo_->setEnabled(ctx_.undoStack().canRedo());
    }
}

bool EditorWindow::handleShortcut(int key, Qt::KeyboardModifiers mods) {
    // Keys typed while the viewport is engaged (hovered/clicked); WASDQE never get here.
    if (QGuiApplication::mouseButtons() & Qt::RightButton) return false;  // flying
    const bool ctrl = mods & Qt::ControlModifier;
    if (key == Qt::Key_Delete) {
        ctx_.deleteSelected();
    } else if (key == Qt::Key_D && ctrl) {
        ctx_.duplicateSelected();
    } else if (key == Qt::Key_Z && ctrl) {
        (mods & Qt::ShiftModifier) ? ctx_.undoStack().redo() : ctx_.undoStack().undo();
    } else if (key == Qt::Key_Y && ctrl) {
        ctx_.undoStack().redo();
    } else if (key == Qt::Key_F && !ctrl) {
        client_->frameSelection();
    } else if (key == Qt::Key_G && !ctrl) {
        grid_->toggle();
    } else if (key == Qt::Key_1 && !ctrl) {
        moveTool_->trigger();
    } else if (key == Qt::Key_2 && !ctrl) {
        rotateTool_->trigger();
    } else if (key == Qt::Key_3 && !ctrl) {
        scaleTool_->trigger();
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
                                "<tr><td><b>W A S D / arrows</b></td><td>Fly (mouse over or click the viewport)</td></tr>"
                                "<tr><td><b>Q / E</b></td><td>Down / up</td></tr>"
                                "<tr><td><b>Shift</b></td><td>Move faster</td></tr>"
                                "<tr><td><b>Right mouse + drag</b></td><td>Look around</td></tr>"
                                "<tr><td><b>Right mouse + wheel</b></td><td>Change fly speed</td></tr>"
                                "<tr><td><b>Middle mouse + drag</b></td><td>Pan</td></tr>"
                                "<tr><td><b>Wheel</b></td><td>Dolly</td></tr>"
                                "<tr><td><b>Left click</b></td><td>Select</td></tr>"
                                "<tr><td><b>1 / 2 / 3</b></td><td>Move / rotate / scale gizmo (Ctrl snaps)</td></tr>"
                                "<tr><td><b>Esc</b></td><td>Clear selection</td></tr>"
                                "<tr><td><b>F</b></td><td>Frame selection</td></tr>"
                                "<tr><td><b>G</b></td><td>Toggle grid</td></tr>"
                                "<tr><td><b>Ctrl+Z / Ctrl+Shift+Z</b></td><td>Undo / redo</td></tr>"
                                "<tr><td><b>F5 / F6 / Shift+F5</b></td><td>Play / Pause / Stop</td></tr>"
                                "<tr><td><b>F8</b></td><td>Toggle scene camera in play mode</td></tr>"
                                "</table>"));
}

bool EditorWindow::prepareToClose() {
    if (!maybeSave()) return false;
    stop();
    return true;
}

void EditorWindow::closeEvent(QCloseEvent* event) {
    if (collabServer_ && collabServer_->clientCount() > 1 &&
        QMessageBox::question(this, tr("End Session"),
                              tr("You are hosting a live session. Closing ends it for everyone. Continue?")) !=
            QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    leaveSession();
    stop();
    saveLayout();
    event->accept();
}

// ---- Live collaboration -------------------------------------------------------------------

void EditorWindow::hostSession() {
    if (collabClient_ || editScene_ == nullptr) return;
    collab::HostSessionDialog dialog(QString::fromStdString(project_.name()), this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (auto started = startHosting({dialog.port(), dialog.displayName(), dialog.password(), dialog.publicAddress()});
        !started) {
        QMessageBox::warning(this, tr("Cannot Host Session"),
                             tr("%1\n\nIs another program (or another Lumora) using port %2? Pick a different port.")
                                 .arg(QString::fromStdString(started.error().message))
                                 .arg(dialog.port()));
    }
}

Result<> EditorWindow::startHosting(const HostOptions& host) {
    if (collabClient_ || editScene_ == nullptr) return makeError("already in a session");
    stop();
    const QString projectName = QString::fromStdString(project_.name());
    collab::CollabServer::Options options;
    options.port = host.port;
    options.password = host.password;
    options.projectName = projectName;
    options.sessionName = QStringLiteral("%1 · %2").arg(projectName, QString::fromStdString(editScene_->name()));
    options.projectRoot = app::toQString(project_.rootDir());
    // The same certificate every time, so guests who confirmed it once keep trusting this host.
    auto identity = collab::loadOrCreateHostIdentity(collab::defaultIdentityFolder());
    if (!identity) return std::unexpected(identity.error());
    options.identity = *identity;
    auto server = std::make_unique<collab::CollabServer>();
    if (auto started = server->start(options, SceneSerializer::toJson(*editScene_)); !started) return started;
    collabServer_ = std::move(server);
    collabClient_ = std::make_unique<collab::CollabClient>();
    collabClient_->setProjectRoot(options.projectRoot);
    const QString publicAddress = host.publicAddress;
    connect(collabClient_.get(), &collab::CollabClient::joined, this,
            [this, publicAddress](const nlohmann::json& scene, quint64 seq) { beginSession(true, publicAddress, scene, seq); });
    connect(collabClient_.get(), &collab::CollabClient::joinFailed, this, [this](const QString& reason) {
        retireClient();
        collabServer_.reset();
        updateSessionUi();
        QTimer::singleShot(0, this, [this, reason] { QMessageBox::warning(this, tr("Cannot Host Session"), reason); });
    });
    collabClient_->connectTo({QStringLiteral("127.0.0.1"), collabServer_->port(), options.identity.fingerprint}, host.name,
                             host.password);
    updateSessionUi();
    return {};
}

void EditorWindow::attachSession(std::unique_ptr<collab::CollabClient> client, const nlohmann::json& scene, quint64 seq) {
    leaveSession();
    collabClient_ = std::move(client);
    collabClient_->setProjectRoot(app::toQString(project_.rootDir()));
    beginSession(false, QString(), scene, seq);
}

void EditorWindow::beginSession(bool hosting, const QString& publicAddress, const nlohmann::json& scene, quint64 seq) {
    stop();
    collab::CollabClient* client = collabClient_.get();
    client->disconnect(this);
    auto applier = [this](const ScenePatch& patch) {
        if (simulation_) {
            (void)patch.applyTo(*editScene_);  // play mode shows a copy; the edit scene still follows
        } else {
            ctx_.applyPatch(patch);
        }
    };
    collabSession_ = std::make_unique<collab::CollabSession>(*client, *editScene_, applier,
                                                             app::toQString(project_.rootDir()));
    collabSession_->poseSource = [this] {
        const glm::mat4 inv = glm::inverse(client_->camera().view(1.0f).view);
        const Uuid selected = ctx_.selectedUuid();
        return collab::CollabSession::Pose{glm::vec3(inv[3]), -glm::normalize(glm::vec3(inv[2])),
                                           selected.isValid() ? QString::fromStdString(selected.toString()) : QString()};
    };
    connect(collabSession_.get(), &collab::CollabSession::remoteChange, this, [this] {
        ctx_.setDirty(true);
        updateRemoteMarkers();
    });
    connect(collabSession_.get(), &collab::CollabSession::assetsArrived, this, [this] {
        assets_.forgetFailures();
        if (render::RenderContext* rc = app_.renderContext()) {
            rc->meshes().forgetFailures();
            rc->textures().forgetFailures();
        }
    });
    connect(collabSession_.get(), &collab::CollabSession::resyncing, this,
            [this] { statusBar()->showMessage(tr("Re-syncing the scene with the host…"), 3000); });
    connect(client, &collab::CollabClient::participantsChanged, this, &EditorWindow::updateRemoteMarkers);
    connect(client, &collab::CollabClient::participantsChanged, this, &EditorWindow::updateSessionUi);
    connect(client, &collab::CollabClient::presenceChanged, this, &EditorWindow::updateRemoteMarkers);
    connect(client, &collab::CollabClient::left, this, [this](const QString& reason) { leaveSession(reason); });

    collabSession_->begin(scene, seq);
    std::optional<collab::CollabPanel::HostInfo> host;
    if (hosting && collabServer_) host = collab::CollabPanel::HostInfo{collabServer_->port(), publicAddress, collabServer_->fingerprint()};
    collabPanel_->attach(client, host);
    collabDock_->show();
    collabDock_->raise();
    updateSessionUi();
    updateTitle();
    statusBar()->showMessage(hosting ? tr("Hosting a live session.") : tr("Joined the live session."), 4000);
}

void EditorWindow::retireClient() {
    if (!collabClient_) return;
    collabClient_->disconnect(this);
    collabClient_->disconnectFromHost();
    collabClient_.release()->deleteLater();  // we may be inside one of its signals
}

void EditorWindow::leaveSession(const QString& note) {
    if (!collabClient_ && !collabServer_) return;
    collabSession_.reset();
    retireClient();
    collabServer_.reset();
    client_->remoteMarkers.clear();
    if (collabPanel_) collabPanel_->detach(note);
    if (!note.isEmpty()) statusBar()->showMessage(note, 6000);
    updateSessionUi();
}

void EditorWindow::updateSessionUi() {
    const bool busy = collabClient_ != nullptr;  // connecting or in a session
    const bool live = inSession();
    hostSession_->setEnabled(!busy);
    joinSession_->setEnabled(!busy);
    leaveSession_->setEnabled(busy);
    newScene_->setEnabled(!live);
    openScene_->setEnabled(!live);
    if (live && collabClient_) {
        const int people = static_cast<int>(collabClient_->participants().size());
        statusCollab_->setText(collabServer_ ? tr("● HOSTING · %n person(s)", nullptr, people)
                                             : tr("● LIVE · %n person(s)", nullptr, people));
        statusCollab_->show();
    } else {
        statusCollab_->hide();
    }
}

void EditorWindow::updateRemoteMarkers() {
    client_->remoteMarkers.clear();
    if (!collabClient_) return;
    for (const auto& [id, p] : collabClient_->participants()) {
        if (id == collabClient_->id()) continue;
        EditorViewportClient::RemoteMarker marker;
        marker.position = p.position;
        marker.forward = p.forward;
        marker.hasPose = p.hasPose;
        marker.color = glm::vec4(static_cast<float>(p.color.redF()), static_cast<float>(p.color.greenF()),
                                 static_cast<float>(p.color.blueF()), 1.0f);
        if (auto uuid = Uuid::parse(p.selection.toStdString())) marker.selection = *uuid;
        client_->remoteMarkers.push_back(marker);
    }
}

// ---- Gameplay scripts ------------------------------------------------------------------------

void EditorWindow::setUpScripts() {
    scripts_ = std::make_unique<app::ScriptBuilder>(app::toQString(project_.rootDir()), this);
    connect(scripts_.get(), &app::ScriptBuilder::buildStarted, this,
            [this] { statusBar()->showMessage(tr("Building scripts…")); });
    connect(scripts_.get(), &app::ScriptBuilder::buildFinished, this, &EditorWindow::onScriptsBuilt);
    scripts_->setWatching(true);  // saving a script rebuilds and reloads the game
    if (!scripts_->hasScripts()) return;
    // Load the last build right away; rebuild in the background in case scripts changed since.
    if (QFileInfo::exists(scripts_->libraryPath())) {
        auto module = ScriptModule::load(app::toPath(scripts_->libraryPath()), app::toPath(scripts_->loadDir()));
        if (module) {
            gameModule_ = std::move(*module);
        } else {
            log::warn("Script", "{}", module.error().message);
        }
    }
    scripts_->build();
}

void EditorWindow::onScriptsBuilt(bool ok, const QString& output) {
    if (!ok) {
        statusBar()->showMessage(tr("Script build failed - see the Console"), 8000);
        const QString summary = output.trimmed().right(1500);
        log::error("Script", "build failed:\n{}", summary.toStdString());
        return;
    }
    auto module = ScriptModule::load(app::toPath(scripts_->libraryPath()), app::toPath(scripts_->loadDir()));
    if (!module) {
        statusBar()->showMessage(tr("Scripts built but could not be loaded - see the Console"), 8000);
        log::error("Script", "{}", module.error().message);
        return;
    }
    gameModule_ = std::move(*module);
    if (simulation_) {
        simulation_->setScripts(gameModule_, &viewport_->input());  // hot reload while playing
        statusBar()->showMessage(tr("Scripts reloaded (%1 behaviours running)").arg(simulation_->scriptInstances()), 4000);
    } else {
        statusBar()->showMessage(tr("Scripts built: %1 behaviours").arg(gameModule_->behaviours().size()), 4000);
    }
}

void EditorWindow::newBehaviour() {
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New Behaviour"), tr("Class name (C++ identifier):"),
                                               QLineEdit::Normal, QStringLiteral("MyBehaviour"), &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty()) return;
    QString error;
    if (!scripts_->createBehaviour(name, &error)) {
        QMessageBox::warning(this, tr("New Behaviour"), error);
        return;
    }
    const QString file = QDir(scripts_->scriptsDir()).filePath(name + QStringLiteral(".cpp"));
    log::info("Script", "created {}", file.toStdString());
    QDesktopServices::openUrl(QUrl::fromLocalFile(file));  // the user's code editor
    scripts_->build();
}

void EditorWindow::exportGameDialog() {
    if (ctx_.undoStack().isClean() == false || scenePath_.isEmpty()) {
        const auto answer = QMessageBox::question(this, tr("Export Game"),
                                                  tr("The exported game uses the saved scene. Save it now?"));
        if (answer == QMessageBox::Yes) saveScene();
    }
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Export Game To (an empty folder)"));
    if (folder.isEmpty()) return;
    if (scripts_ && scripts_->hasScripts() && !QFileInfo::exists(scripts_->libraryPath())) {
        QMessageBox::warning(this, tr("Export Game"), tr("Build the scripts first (Scripts > Build Scripts)."));
        return;
    }
    auto launcher = app::exportGame({project_.rootDir(), project_.name(), app::toPath(folder), {}});
    if (!launcher) {
        QMessageBox::critical(this, tr("Export failed"), QString::fromStdString(launcher.error().message));
        return;
    }
    QMessageBox::information(this, tr("Export Game"),
                             tr("Exported. Start the game with %1.").arg(app::toQString(*launcher)));
}

// ---- Prefabs ---------------------------------------------------------------------------------

void EditorWindow::saveSelectionAsPrefab() {
    const entt::entity selected = ctx_.selectedEntity();
    if (ctx_.scene() == nullptr || selected == entt::null) {
        QMessageBox::information(this, tr("Save as Prefab"), tr("Select the entity to save (its children come along)."));
        return;
    }
    const QString name = QString::fromStdString(ctx_.scene()->nameOf(selected));
    const QString suggested = app::toQString(project_.contentDir() / "Prefabs" / (name.toStdString() + ".lprefab"));
    QDir().mkpath(app::toQString(project_.contentDir() / "Prefabs"));
    const QString file = QFileDialog::getSaveFileName(this, tr("Save as Prefab"), suggested, tr("Lumora prefabs (*.lprefab)"));
    if (file.isEmpty()) return;
    std::filesystem::path path = app::toPath(file);
    if (path.extension() != ".lprefab") path += ".lprefab";
    if (auto saved = savePrefab(*ctx_.scene(), selected, path); !saved) {
        QMessageBox::critical(this, tr("Save as Prefab"), QString::fromStdString(saved.error().message));
        return;
    }
    statusBar()->showMessage(tr("Saved prefab %1").arg(app::toQString(path.filename())), 4000);
}

entt::entity EditorWindow::addPrefabToScene(const std::filesystem::path& path) {
    auto prefab = loadPrefab(path);
    if (!prefab) {
        QMessageBox::critical(this, tr("Cannot load prefab"), QString::fromStdString(prefab.error().message));
        return entt::null;
    }
    const FlyCameraController& cam = client_->camera();
    glm::vec3 spot = cam.position() + cam.forward() * 5.0f;
    if (cam.forward().y < -0.05f) {
        const float t = -cam.position().y / cam.forward().y;
        if (t > 0.0f && t < 60.0f) spot = cam.position() + cam.forward() * t;
    }
    entt::entity root = entt::null;
    QString error;
    ctx_.edit(tr("Add %1").arg(app::toQString(path.stem())), [&](Scene& scene) {
        auto created = instantiatePrefab(scene, *prefab);
        if (!created) {
            error = QString::fromStdString(created.error().message);
            return;
        }
        root = *created;
        scene.registry().get<TransformComponent>(root).position = spot;
    });
    if (!error.isEmpty()) QMessageBox::critical(this, tr("Cannot place prefab"), error);
    if (root != entt::null) ctx_.select(editScene_->uuidOf(root));
    return root;
}

} // namespace lumora::editor
