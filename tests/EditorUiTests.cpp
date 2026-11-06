// Drives the real Hub and Editor widgets (offscreen QPA). Rendering is not exercised here
// (the offscreen platform has no Vulkan surfaces); see RendererTests for GPU coverage.
#include "TestUtils.hpp"

#include "collab/CollabClient.hpp"
#include "collab/CollabServer.hpp"
#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/VulkanViewport.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorWindow.hpp"
#include "hub/HubWindow.hpp"
#include "main/Application.hpp"
#include "project/Project.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"
#include "runtime/Simulation.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QAction>
#include <QDockWidget>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QToolButton>
#include <QTreeWidget>

using namespace lumora;

namespace {

void processEvents() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

bool waitFor(const std::function<bool()>& done, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return true;
}

entt::entity findNamed(const Scene& scene, std::string_view name) {
    entt::entity found = entt::null;
    scene.forEachDepthFirst([&](entt::entity e, int) {
        if (scene.nameOf(e) == name) found = e;
    });
    return found;
}

class EditorUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = Project::create(dir_.path() / "Demo", "Demo", ProjectTemplate::BasicScene);
        ASSERT_TRUE(created.has_value()) << created.error().message;
        descriptor_ = app::toQString(created->descriptorPath());
        context_ = std::make_unique<app::AppContext>(app::AppOptions{});
        application_ = std::make_unique<app::Application>(*context_);
    }
    void TearDown() override {
        if (auto* editor = application_ ? application_->editor() : nullptr) {
            editor->context().setDirty(false);  // no save prompt
        }
        application_.reset();
        context_.reset();
    }

    editor::EditorWindow* openEditor() {
        EXPECT_TRUE(application_->openEditor(descriptor_));
        processEvents();
        return application_->editor();
    }

    test::TempDir dir_;
    QString descriptor_;
    std::unique_ptr<app::AppContext> context_;
    std::unique_ptr<app::Application> application_;
};

} // namespace

TEST_F(EditorUiTest, HubToEditorTransitionInOneProcess) {
    application_->showHub();
    processEvents();
    ASSERT_NE(application_->hub(), nullptr);
    EXPECT_TRUE(application_->hub()->isVisible());

    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(editor->isVisible());
    processEvents();
    EXPECT_EQ(application_->hub(), nullptr) << "hub closes once the editor is open";
    EXPECT_EQ(editor->project().name(), "Demo");
    ASSERT_NE(editor->editScene(), nullptr);
    EXPECT_GT(editor->editScene()->entityCount(), 3u);

    // Recent projects were updated.
    ASSERT_FALSE(context_->recentProjects().entries().empty());
    EXPECT_EQ(context_->recentProjects().entries().front().name, "Demo");

    // Closing the project returns to the hub.
    application_->closeEditor();
    processEvents();
    ASSERT_NE(application_->hub(), nullptr);
    EXPECT_TRUE(application_->hub()->isVisible());
}

TEST_F(EditorUiTest, OpeningInvalidProjectFallsBackToHub) {
    // QMessageBox would block; the error path is exercised through Project::load directly.
    EXPECT_FALSE(Project::load(dir_.path() / "missing").has_value());
}

TEST_F(EditorUiTest, HierarchyReflectsCreateRenameDelete) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor::EditorContext& ctx = editor->context();
    auto* tree = editor->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    auto countItems = [&] {
        int n = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) ++n;
        return n;
    };
    const int before = countItems();
    EXPECT_EQ(before, static_cast<int>(editor->editScene()->entityCount()));

    const entt::entity e = ctx.createEntity(QStringLiteral("Probe"), [](Scene& s, entt::entity id) {
        s.registry().emplace<MeshRendererComponent>(id);
    });
    processEvents();
    EXPECT_EQ(countItems(), before + 1);
    EXPECT_EQ(ctx.selectedEntity(), e) << "new entities are selected";
    EXPECT_TRUE(ctx.isDirty());
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->text(0), QStringLiteral("Probe"));

    ctx.renameEntity(editor->editScene()->uuidOf(e), QStringLiteral("Renamed"));
    processEvents();
    EXPECT_EQ(editor->editScene()->nameOf(e), "Renamed");
    EXPECT_EQ(tree->currentItem()->text(0), QStringLiteral("Renamed"));

    ctx.duplicateSelected();
    processEvents();
    EXPECT_EQ(countItems(), before + 2);
    ctx.deleteSelected();
    processEvents();
    EXPECT_EQ(countItems(), before + 1);
    EXPECT_EQ(ctx.selectedEntity(), entt::entity{entt::null});
}

TEST_F(EditorUiTest, ReparentAndSaveRoundTrip) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor::EditorContext& ctx = editor->context();
    Scene& scene = *editor->editScene();

    const entt::entity parent = ctx.createEntity(QStringLiteral("Parent"));
    const entt::entity child = ctx.createEntity(QStringLiteral("Child"));
    ASSERT_TRUE(ctx.reparent(scene.uuidOf(child), scene.uuidOf(parent), -1));
    EXPECT_EQ(scene.parentOf(child), parent);
    scene.registry().get<TransformComponent>(child).position = {1.0f, 2.0f, 3.0f};

    ASSERT_TRUE(editor->saveScene());
    EXPECT_FALSE(ctx.isDirty());
    auto loaded = SceneSerializer::load(app::toPath(editor->scenePath()));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    const entt::entity loadedChild = (*loaded)->findByUuid(scene.uuidOf(child));
    ASSERT_NE(loadedChild, entt::entity{entt::null});
    EXPECT_EQ((*loaded)->uuidOf((*loaded)->parentOf(loadedChild)), scene.uuidOf(parent));
    EXPECT_EQ((*loaded)->registry().get<TransformComponent>(loadedChild).position, glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_F(EditorUiTest, PlayModeRunsOnACopyAndStopRestores) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor::EditorContext& ctx = editor->context();
    Scene* editScene = editor->editScene();
    // The BasicScene template has a cube with a RotatorComponent.
    entt::entity spinner = entt::null;
    for (auto [e, rot] : editScene->registry().view<RotatorComponent>().each()) spinner = e;
    ASSERT_NE(spinner, entt::entity{entt::null});
    const Uuid spinnerId = editScene->uuidOf(spinner);
    const glm::vec3 before = editScene->registry().get<TransformComponent>(spinner).rotationDeg;

    editor->play();
    EXPECT_TRUE(editor->isPlaying());
    EXPECT_TRUE(ctx.isPlaying());
    ASSERT_NE(ctx.scene(), editScene) << "play mode simulates a clone";

    // Advance the simulation through the same path the viewport uses.
    Scene* playScene = ctx.scene();
    Simulation sim(*playScene);
    for (int i = 0; i < 30; ++i) sim.step();
    const entt::entity playSpinner = playScene->findByUuid(spinnerId);
    EXPECT_NE(playScene->registry().get<TransformComponent>(playSpinner).rotationDeg, before);

    editor->togglePause();
    EXPECT_TRUE(editor->isPaused());
    editor->stop();
    EXPECT_FALSE(editor->isPlaying());
    EXPECT_EQ(ctx.scene(), editScene);
    EXPECT_EQ(editScene->registry().get<TransformComponent>(spinner).rotationDeg, before)
        << "play-mode changes never leak into the edited scene";
    EXPECT_FALSE(ctx.isDirty());
}

TEST_F(EditorUiTest, InspectorBuildsSectionsFromReflection) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor->resize(1600, 960);
    Scene& scene = *editor->editScene();
    entt::entity cube = entt::null;
    for (auto [e, rot] : scene.registry().view<RotatorComponent>().each()) cube = e;
    ASSERT_NE(cube, entt::entity{entt::null});
    editor->context().select(scene.uuidOf(cube));
    processEvents();

    QStringList sections;
    for (QToolButton* b : editor->findChildren<QToolButton*>()) {
        if (b->objectName() == QLatin1String("sectionTitle") && b->isVisible()) sections << b->text();
    }
    EXPECT_TRUE(sections.contains(QStringLiteral("Transform"))) << sections.join(',').toStdString();
    EXPECT_TRUE(sections.contains(QStringLiteral("Mesh Renderer")));
    EXPECT_TRUE(sections.contains(QStringLiteral("Rotator")));
    EXPECT_FALSE(sections.contains(QStringLiteral("Light")));

    if (const QString dir = qEnvironmentVariable("LUMORA_TEST_SNAPSHOT_DIR"); !dir.isEmpty()) {
        editor->grab().save(dir + QStringLiteral("/inspector.png"));
    }
}

TEST_F(EditorUiTest, UndoRedoStructuralEdits) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor::EditorContext& ctx = editor->context();
    Scene& scene = *editor->editScene();
    const std::size_t initial = scene.entityCount();
    EXPECT_FALSE(ctx.isDirty());

    const entt::entity e = ctx.createEntity(QStringLiteral("Undoable"));
    const Uuid id = scene.uuidOf(e);
    EXPECT_EQ(scene.entityCount(), initial + 1);
    EXPECT_TRUE(ctx.isDirty());

    ctx.undoStack().undo();
    EXPECT_EQ(scene.entityCount(), initial);
    EXPECT_EQ(scene.findByUuid(id), entt::entity{entt::null});
    EXPECT_FALSE(ctx.isDirty()) << "undoing back to the saved state is clean";
    EXPECT_FALSE(ctx.selectedUuid().isValid()) << "selection of a removed entity is cleared";

    ctx.undoStack().redo();
    EXPECT_EQ(scene.entityCount(), initial + 1);
    EXPECT_NE(scene.findByUuid(id), entt::entity{entt::null}) << "redo restores the same UUID";

    // Delete a subtree with components, then undo restores everything.
    entt::entity props = entt::null;
    for (entt::entity root : scene.roots()) {
        if (scene.nameOf(root) == "Props") props = root;
    }
    ASSERT_NE(props, entt::entity{entt::null});
    const Uuid propsId = scene.uuidOf(props);
    const std::size_t children = scene.childrenOf(props).size();
    ctx.select(propsId);
    ctx.deleteSelected();
    EXPECT_EQ(scene.findByUuid(propsId), entt::entity{entt::null});
    ctx.undoStack().undo();
    const entt::entity restored = scene.findByUuid(propsId);
    ASSERT_NE(restored, entt::entity{entt::null});
    EXPECT_EQ(scene.childrenOf(restored).size(), children);

    // Rejected re-parent (cycle) records nothing.
    const int depth = ctx.undoStack().index();
    EXPECT_FALSE(ctx.reparent(propsId, scene.uuidOf(scene.childrenOf(restored).front()), -1));
    EXPECT_EQ(ctx.undoStack().index(), depth);
}

TEST_F(EditorUiTest, FieldEditsMergeIntoOneUndoStep) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor::EditorContext& ctx = editor->context();
    Scene& scene = *editor->editScene();
    const entt::entity e = scene.roots().front();
    const Uuid id = scene.uuidOf(e);
    const ComponentDesc& transform = *ComponentRegistry::builtin().find("Transform");
    const FieldDesc& position = *transform.findField("position");
    const glm::vec3 original = scene.registry().get<TransformComponent>(e).position;

    // Scrubbing produces a burst of edits to the same field.
    for (int i = 1; i <= 5; ++i) {
        ctx.setField(id, transform, position, FieldValue(original + glm::vec3(static_cast<float>(i), 0.0f, 0.0f)));
    }
    EXPECT_EQ(scene.registry().get<TransformComponent>(e).position.x, original.x + 5.0f);
    EXPECT_EQ(ctx.undoStack().count(), 1) << "consecutive edits of one field merge";
    ctx.undoStack().undo();
    EXPECT_EQ(scene.registry().get<TransformComponent>(e).position, original);

    // Environment edits are undoable too.
    const float exposure = scene.environment().exposure;
    ctx.setEnvironment(QStringLiteral("Exposure"), [](EnvironmentSettings& env) { env.exposure = 3.0f; });
    EXPECT_FLOAT_EQ(scene.environment().exposure, 3.0f);
    ctx.undoStack().undo();
    EXPECT_FLOAT_EQ(scene.environment().exposure, exposure);
}

TEST_F(EditorUiTest, PlayModeEditsAreNotRecorded) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor::EditorContext& ctx = editor->context();
    editor->play();
    ctx.createEntity(QStringLiteral("Transient"));
    EXPECT_EQ(ctx.undoStack().count(), 0);
    editor->stop();
    EXPECT_FALSE(ctx.isDirty());
}

TEST_F(EditorUiTest, KeyboardReachingTheMainWindowFliesTheViewportCamera) {
    // On Wayland the embedded viewport never gets keyboard focus: keys arrive at the top-level
    // window. While the viewport is hovered (or was clicked) it must still take WASD.
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    app::VulkanViewport* viewport = nullptr;
    for (QWindow* w : QGuiApplication::allWindows()) {
        if (auto* v = qobject_cast<app::VulkanViewport*>(w)) viewport = v;
    }
    ASSERT_NE(viewport, nullptr);
    QWindow* top = editor->windowHandle();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(qobject_cast<QLineEdit*>(QApplication::focusWidget()), nullptr)
        << "the editor must not open with a text field holding the keyboard";

    auto send = [&](QEvent::Type type, int key) {
        QKeyEvent e(type, key, Qt::NoModifier);
        return QCoreApplication::sendEvent(top, &e);
    };

    // Not engaged: keys belong to the widgets.
    send(QEvent::KeyPress, Qt::Key_W);
    EXPECT_FALSE(viewport->input().isDown(Key::W));
    send(QEvent::KeyRelease, Qt::Key_W);

    QEnterEvent enter(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
    QCoreApplication::sendEvent(viewport, &enter);
    send(QEvent::KeyPress, Qt::Key_W);
    EXPECT_TRUE(viewport->input().isDown(Key::W));
    send(QEvent::KeyRelease, Qt::Key_W);
    EXPECT_FALSE(viewport->input().isDown(Key::W));

    // Gizmo tools live on 1/2/3 (E flies up).
    QAction* rotate = nullptr;
    for (QAction* a : editor->findChildren<QAction*>()) {
        if (a->text() == QStringLiteral("Rotate (2)")) rotate = a;
    }
    ASSERT_NE(rotate, nullptr);
    send(QEvent::KeyPress, Qt::Key_2);
    EXPECT_TRUE(rotate->isChecked());

    // Leaving the viewport releases held keys so nothing stays stuck.
    send(QEvent::KeyPress, Qt::Key_D);
    EXPECT_TRUE(viewport->input().isDown(Key::D));
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(viewport, &leave);
    EXPECT_FALSE(viewport->input().isDown(Key::D));
    send(QEvent::KeyPress, Qt::Key_S);
    EXPECT_FALSE(viewport->input().isDown(Key::S));
    send(QEvent::KeyRelease, Qt::Key_S);

    // Typing in a text field never moves the camera, even with the pointer over the viewport.
    auto* field = editor->findChild<QLineEdit*>();
    ASSERT_NE(field, nullptr);
    field->setFocus();
    processEvents();
    ASSERT_EQ(QApplication::focusWidget(), field);
    QCoreApplication::sendEvent(viewport, &enter);
    send(QEvent::KeyPress, Qt::Key_A);
    EXPECT_FALSE(viewport->input().isDown(Key::A));
}

TEST_F(EditorUiTest, TwoEditorsShareASceneLive) {
    editor::EditorWindow* host = openEditor();
    ASSERT_NE(host, nullptr);
    ASSERT_TRUE(host->startHosting({0, QStringLiteral("Host"), {}, {}}).has_value());
    ASSERT_TRUE(waitFor([&] { return host->inSession(); }));

    // A guest editor with a project of its own joins over loopback.
    auto guestProject = Project::create(dir_.path() / "Guest", "Guest", ProjectTemplate::Empty);
    ASSERT_TRUE(guestProject.has_value()) << guestProject.error().message;
    editor::EditorWindow guest(*context_, std::move(*guestProject));
    auto client = std::make_unique<collab::CollabClient>();
    collab::CollabClient* raw = client.get();
    QObject::connect(raw, &collab::CollabClient::joined, &guest, [&](const nlohmann::json& scene, quint64 seq) {
        guest.attachSession(std::move(client), scene, seq);  // synchronously, like Application
    });
    raw->connectTo({QStringLiteral("127.0.0.1"), host->sessionServer()->port(), host->sessionServer()->fingerprint()},
                   QStringLiteral("Guest"), {});
    ASSERT_TRUE(waitFor([&] { return guest.inSession(); }));
    EXPECT_EQ(SceneSerializer::toJson(*guest.editScene()), SceneSerializer::toJson(*host->editScene()));

    // Host creates, guest renames, host undoes its creation: everyone follows.
    host->context().createEntity(QStringLiteral("Shared Cube"));
    ASSERT_TRUE(waitFor([&] { return findNamed(*guest.editScene(), "Shared Cube") != entt::null; }));
    EXPECT_TRUE(guest.context().isDirty());
    const Uuid cube = guest.editScene()->uuidOf(findNamed(*guest.editScene(), "Shared Cube"));
    guest.context().renameEntity(cube, QStringLiteral("Guest's Cube"));
    ASSERT_TRUE(waitFor([&] { return findNamed(*host->editScene(), "Guest's Cube") != entt::null; }));

    // Both cameras are visible to the other side.
    ASSERT_TRUE(waitFor([&] {
        for (const auto& [id, p] : host->sessionClient()->participants()) {
            if (p.name == QStringLiteral("Guest") && p.hasPose) return true;
        }
        return false;
    }));

    host->context().undoStack().undo();  // the host undoes its creation; the guest's rename goes with it
    ASSERT_TRUE(waitFor([&] { return findNamed(*guest.editScene(), "Guest's Cube") == entt::null; }));

    guest.leaveSession();
    ASSERT_TRUE(waitFor([&] { return host->sessionClient()->participants().size() == 1; }));
    host->leaveSession();
    EXPECT_FALSE(host->inSession());
}

TEST_F(EditorUiTest, PointerPassThroughReachesTheViewport) {
    // On Wayland the viewport lets the pointer through to its container (so the cursor can be
    // locked on the main surface); the container's mouse events must drive the viewport.
    qputenv("LUMORA_POINTER_PASSTHROUGH", "1");
    editor::EditorWindow* editor = openEditor();
    qunsetenv("LUMORA_POINTER_PASSTHROUGH");
    ASSERT_NE(editor, nullptr);
    app::VulkanViewport* viewport = nullptr;
    for (QWindow* w : QGuiApplication::allWindows()) {
        if (auto* v = qobject_cast<app::VulkanViewport*>(w)) viewport = v;
    }
    ASSERT_NE(viewport, nullptr);
    EXPECT_TRUE(viewport->flags().testFlag(Qt::WindowTransparentForInput));
    QWidget* container = nullptr;
    for (QWidget* w : editor->findChildren<QWidget*>()) {
        if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("QWindowContainer")) container = w;
    }
    ASSERT_NE(container, nullptr);

    const QPointF at(40, 30);
    QMouseEvent press(QEvent::MouseButtonPress, at, container->mapToGlobal(at), Qt::RightButton, Qt::RightButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(container, &press);
    EXPECT_TRUE(viewport->input().isDown(MouseButton::Right)) << "right press reached the viewport";
    EXPECT_EQ(container->cursor().shape(), Qt::BlankCursor) << "the cursor hides while looking";
    QMouseEvent release(QEvent::MouseButtonRelease, at, container->mapToGlobal(at), Qt::RightButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(container, &release);
    EXPECT_FALSE(viewport->input().isDown(MouseButton::Right));
    EXPECT_NE(container->cursor().shape(), Qt::BlankCursor);

    // Hovering the container engages the keyboard like hovering the viewport itself.
    QEnterEvent enter(at, at, container->mapToGlobal(at));
    QCoreApplication::sendEvent(container, &enter);
    QKeyEvent w(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    QCoreApplication::sendEvent(editor->windowHandle(), &w);
    EXPECT_TRUE(viewport->input().isDown(Key::W));
    QKeyEvent wUp(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
    QCoreApplication::sendEvent(editor->windowHandle(), &wUp);
}

// ---- Content Browser and code editor ---------------------------------------------------------

#include "editor/CodeEditorPanel.hpp"
#include <QDir>
#include "editor/ContentBrowserPanel.hpp"

#include <QListView>
#include <QListWidget>
#include <QTreeView>

#include <fstream>

TEST(ContentBrowser, ShowsTheWholeProjectAndOpensFiles) {
    test::TempDir dir;
    const auto root = dir.path();
    std::filesystem::create_directories(root / "Content" / "Models");
    std::filesystem::create_directories(root / "Scripts");
    std::filesystem::create_directories(root / "Saved" / "Cache");
    std::ofstream(root / "Scripts" / "Game.cpp") << "int x;\n";
    editor::ContentBrowserPanel browser(app::toQString(root));
    EXPECT_EQ(browser.currentFolder(), app::toQString(root / "Content")) << "starts in Content";
    browser.navigate(app::toQString(root / "Content" / "Models"));
    EXPECT_TRUE(browser.currentFolder().endsWith(QStringLiteral("Content/Models")));
    browser.navigate(QStringLiteral("/"));  // outside the project
    EXPECT_EQ(browser.currentFolder(), app::toQString(root)) << "never leaves the project";

    // Double-clicking a script asks for the code editor.
    QStringList code;
    QObject::connect(&browser, &editor::ContentBrowserPanel::openCodeRequested, [&](const QString& p) { code << p; });
    browser.navigate(app::toQString(root / "Scripts"));
    auto* view = browser.findChild<QListView*>();
    ASSERT_NE(view, nullptr);
    QModelIndex script;
    const bool listed = waitFor([&] {  // the file model fills in on a worker thread
        const QModelIndex rootIndex = view->rootIndex();
        for (int r = 0; r < view->model()->rowCount(rootIndex); ++r) {
            const QModelIndex idx = view->model()->index(r, 0, rootIndex);
            if (idx.data().toString() == QStringLiteral("Game.cpp")) script = idx;
        }
        return script.isValid();
    });
    ASSERT_TRUE(listed);
    emit view->activated(script);
    ASSERT_EQ(code.size(), 1);
    EXPECT_TRUE(code.front().endsWith(QStringLiteral("Scripts/Game.cpp")));

    // The folder tree hides the editor's Saved/ folder.
    auto* tree = browser.findChild<QTreeView*>();
    ASSERT_NE(tree, nullptr);
    const auto topFolders = [&] {
        QStringList names;
        for (int r = 0; r < tree->model()->rowCount(tree->rootIndex()); ++r)
            names << tree->model()->index(r, 0, tree->rootIndex()).data().toString();
        return names;
    };
    ASSERT_TRUE(waitFor([&] { return topFolders().contains(QStringLiteral("Scripts")); })) << "the tree fills in";
    EXPECT_FALSE(topFolders().contains(QStringLiteral("Saved")));
}

TEST(CodeEditor, EditsSavesAndJumpsToCompilerErrors) {
    test::TempDir dir;
    const auto file = dir.path() / "Game.cpp";
    std::ofstream(file) << "int main() {\n    return 0;\n}\n";
    editor::CodeEditorPanel panel;
    editor::CodeEdit* edit = panel.openFile(QString::fromStdString(file.string()));
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(panel.openFile(QString::fromStdString(file.string())), edit) << "a file opens once";
    EXPECT_EQ(panel.openFileCount(), 1);
    EXPECT_TRUE(edit->toPlainText().contains(QStringLiteral("return 0;")));

    edit->moveCursor(QTextCursor::End);
    edit->insertPlainText(QStringLiteral("// changed\n"));
    EXPECT_TRUE(panel.hasUnsavedChanges());
    QKeyEvent save(QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier);
    QCoreApplication::sendEvent(edit, &save);
    EXPECT_FALSE(panel.hasUnsavedChanges());
    std::ifstream in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("// changed"), std::string::npos) << "Ctrl+S wrote the file";

    // GCC and MSVC messages become diagnostics; activating one jumps to the line.
    panel.setBuildResult(false, QString::fromStdString(file.string()) + QStringLiteral(":2:5: error: expected ';'\n") +
                                    QString::fromStdString(file.string()) + QStringLiteral("(3): warning C4100: unused\n") +
                                    QStringLiteral("ninja: build stopped\n"));
    EXPECT_EQ(panel.diagnosticCount(), 2);
    auto* list = panel.findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    emit list->itemActivated(list->item(0));
    EXPECT_EQ(edit->textCursor().blockNumber(), 1) << "line 2";
}

#include "common/ScriptBuilder.hpp"
#include "editor/ScriptApiCatalog.hpp"

#include <QAbstractItemView>
#include <QFile>
#include <QCompleter>
#include <QRegularExpression>

TEST(CodeEditor, CompletionCatalogMatchesTheScriptingSdk) {
    // Every Behaviour method in lumora/Script.hpp is offered, and everything offered exists.
    QFile header(app::ScriptBuilder::sdkIncludeDir() + QStringLiteral("/lumora/Script.hpp"));
    ASSERT_TRUE(header.open(QIODevice::ReadOnly)) << header.fileName().toStdString();
    const QString sdk = QString::fromUtf8(header.readAll());
    const qsizetype classStart = sdk.indexOf(QStringLiteral("class Behaviour {"));
    const qsizetype privateStart = sdk.indexOf(QStringLiteral("\nprivate:"), classStart);
    ASSERT_GT(classStart, 0);
    const QString body = sdk.mid(classStart, privateStart - classStart);
    QSet<QString> catalog;
    for (const auto& entry : editor::scriptApi()) {
        if (entry.scope == editor::ScriptApiEntry::Scope::Behaviour) catalog.insert(QString::fromLatin1(entry.name));
        if (entry.scope != editor::ScriptApiEntry::Scope::Member) {
            EXPECT_TRUE(sdk.contains(QRegularExpression(QStringLiteral("\\b%1\\b").arg(QString::fromLatin1(entry.name)))))
                << entry.name << " is not in the SDK";
        }
    }
    static const QRegularExpression method(QStringLiteral(R"(^ {4}(?:\[\[nodiscard\]\]\s+)?(?:virtual\s+)?[\w:<>]+\s+(\w+)\()"),
                                           QRegularExpression::MultilineOption);
    int methods = 0;
    for (auto it = method.globalMatch(body); it.hasNext();) {
        const QString name = it.next().captured(1);
        if (name == QLatin1String("Behaviour")) continue;
        ++methods;
        EXPECT_TRUE(catalog.contains(name)) << name.toStdString() << " is missing from the completion catalog";
    }
    EXPECT_GT(methods, 30);
}

TEST(CodeEditor, CompletesTheScriptingApiWhileTyping) {
    test::TempDir dir;
    const auto file = dir.path() / "Game.cpp";
    std::ofstream(file) << "void f() {\n";
    editor::CodeEditorPanel panel;
    panel.resize(800, 500);
    panel.show();
    editor::CodeEdit* edit = panel.openFile(QString::fromStdString(file.string()));
    ASSERT_NE(edit, nullptr);
    edit->moveCursor(QTextCursor::End);
    auto type = [&](const QString& text) {
        for (QChar c : text) {
            QKeyEvent press(QEvent::KeyPress, c == QLatin1Char('\n') ? Qt::Key_Return : 0, Qt::NoModifier,
                            QString(c));
            QCoreApplication::sendEvent(edit, &press);
        }
    };
    auto offered = [&] {
        QStringList words;
        QAbstractItemModel* model = edit->completer()->completionModel();
        for (int i = 0; i < model->rowCount(); ++i) words << model->index(i, 0).data(Qt::UserRole + 1).toString();
        return words;
    };
    QCompleter* completer = edit->completer();

    type(QStringLiteral("setPo"));
    ASSERT_TRUE(completer->popup()->isVisible());
    EXPECT_TRUE(offered().contains(QStringLiteral("setPosition")));
    EXPECT_TRUE(offered().contains(QStringLiteral("setPositionOf")));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(completer->popup(), &enter);
    EXPECT_TRUE(edit->toPlainText().endsWith(QStringLiteral("setPosition()")));
    type(QStringLiteral("x"));
    EXPECT_TRUE(edit->toPlainText().endsWith(QStringLiteral("setPosition(x)"))) << "the cursor went between the parentheses";

    edit->moveCursor(QTextCursor::End);
    type(QStringLiteral(";\nkeyDown(Key::"));
    ASSERT_TRUE(completer->popup()->isVisible());
    EXPECT_TRUE(offered().contains(QStringLiteral("Space")));
    EXPECT_FALSE(offered().contains(QStringLiteral("position"))) << "only keys after Key::";
    type(QStringLiteral("Spa"));
    EXPECT_EQ(offered(), QStringList{QStringLiteral("Space")});

    type(QStringLiteral(");\nhit."));
    ASSERT_TRUE(completer->popup()->isVisible());
    EXPECT_TRUE(offered().contains(QStringLiteral("normal")));
    EXPECT_TRUE(offered().contains(QStringLiteral("x")));

    type(QStringLiteral(";\nfloat a = 1."));
    EXPECT_FALSE(completer->popup()->isVisible()) << "no members after a number";
    type(QStringLiteral("5f; // pos"));
    EXPECT_FALSE(completer->popup()->isVisible()) << "nothing in comments";

    QKeyEvent ctrlSpace(QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier);
    type(QStringLiteral("\nsetPositionOf("));  // a word of the file is offered too
    type(QStringLiteral("myVariable);\nmyV"));
    EXPECT_TRUE(offered().contains(QStringLiteral("myVariable")));
    QCoreApplication::sendEvent(edit, &ctrlSpace);
    EXPECT_TRUE(completer->popup()->isVisible());
}

#include "assets/Material.hpp"
#include <QDir>
#include "editor/ContentBrowserPanel.hpp"
#include "editor/EditorViewportClient.hpp"
#include "editor/AddComponentMenu.hpp"
#include "editor/InspectorPanel.hpp"
#include "editor/MaterialEditor.hpp"
#include "editor/SceneHierarchyPanel.hpp"

TEST_F(EditorUiTest, MaterialsAreCreatedEditedAndDroppedOnObjects) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor->resize(1600, 960);
    Scene& scene = *editor->editScene();
    entt::entity cube = entt::null;
    for (auto [e, rot] : scene.registry().view<RotatorComponent>().each()) cube = e;
    ASSERT_TRUE(cube != entt::null);
    const Uuid cubeId = scene.uuidOf(cube);

    // New → Material in the Content Browser: a default .lmat, shown in the Inspector.
    auto* browser = editor->findChild<editor::ContentBrowserPanel*>();
    auto* inspector = editor->findChild<editor::InspectorPanel*>();
    ASSERT_NE(browser, nullptr);
    ASSERT_NE(inspector, nullptr);
    const QString materials = app::toQString(editor->project().contentDir() / "Materials");
    QDir().mkpath(materials);
    browser->navigate(materials);
    const QString paint = browser->createMaterial(QStringLiteral("Paint"));
    ASSERT_FALSE(paint.isEmpty());
    ASSERT_TRUE(loadMaterial(app::toPath(paint)).has_value());
    processEvents();
    EXPECT_EQ(inspector->shownAsset(), paint);
    auto* materialEditor = inspector->findChild<editor::MaterialEditor*>();
    ASSERT_NE(materialEditor, nullptr);
    EXPECT_TRUE(materialEditor->isLoaded());
    EXPECT_EQ(browser->createMaterial(QStringLiteral("Paint")), QString()) << "never overwrites";

    // Dropped on the object under the cursor: one undo step.
    editor->viewportClient().camera().setPosition(glm::vec3(scene.worldMatrix(cube)[3]) + glm::vec3(0.0f, 1.0f, 4.0f));
    editor->viewportClient().camera().lookAt(glm::vec3(scene.worldMatrix(cube)[3]));
    editor->dropOnViewport({paint}, glm::vec2(0.0f));
    auto materialOf = [&] { return scene.registry().get<MeshRendererComponent>(scene.findByUuid(cubeId)).material; };
    EXPECT_EQ(materialOf(), "Content/Materials/Paint.lmat");
    editor->context().undoStack().undo();
    EXPECT_EQ(materialOf(), "");
    editor->context().undoStack().redo();
    EXPECT_EQ(materialOf(), "Content/Materials/Paint.lmat");

    // Selecting the object: its Inspector shows the shared material, the own colour is off.
    editor->context().select(cubeId);
    processEvents();
    EXPECT_TRUE(inspector->shownAsset().isEmpty());
    QStringList sections;
    for (QToolButton* b : inspector->findChildren<QToolButton*>()) {
        if (b->objectName() == QLatin1String("sectionTitle")) sections << b->text();
    }
    EXPECT_TRUE(sections.filter(QStringLiteral("Paint")).size() == 1) << sections.join(',').toStdString();
    EXPECT_NE(inspector->findChild<editor::MaterialEditor*>(), nullptr);

    // Dropped on an entity in the Hierarchy: the sun has no mesh, the floor gets it.
    entt::entity other = entt::null;
    for (auto [e, mesh] : scene.registry().view<MeshRendererComponent>().each()) {
        if (e != cube && mesh.material.empty()) other = e;
    }
    ASSERT_TRUE(other != entt::null);
    auto* hierarchy = editor->findChild<editor::SceneHierarchyPanel*>();
    ASSERT_NE(hierarchy, nullptr);
    emit hierarchy->tree()->filesDropped(scene.uuidOf(other), {paint});
    EXPECT_EQ(scene.registry().get<MeshRendererComponent>(other).material, "Content/Materials/Paint.lmat");
}

TEST_F(EditorUiTest, ScriptsAreAddedFromTheInspectorAndDroppedOnObjects) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor->resize(1600, 960);
    Scene& scene = *editor->editScene();
    entt::entity cube = entt::null;
    for (auto [e, rot] : scene.registry().view<RotatorComponent>().each()) cube = e;
    ASSERT_TRUE(cube != entt::null);
    const Uuid cubeId = scene.uuidOf(cube);
    editor->context().select(cubeId);
    processEvents();

    // Add Component > Scripting > Script: the Inspector rebuilds (deleting the menu) - no crash.
    auto* inspector = editor->findChild<editor::InspectorPanel*>();
    ASSERT_NE(inspector, nullptr);
    QAction* scriptAction = nullptr;
    for (auto* menu : inspector->findChildren<editor::AddComponentMenu*>()) {
        for (QAction* a : menu->actions()) {
            if (a->menu() == nullptr || a->text() != QStringLiteral("Scripting")) continue;
            for (QAction* item : a->menu()->actions()) {
                if (item->text() == QStringLiteral("Script")) scriptAction = item;
            }
        }
    }
    ASSERT_NE(scriptAction, nullptr);
    scriptAction->trigger();
    processEvents();
    entt::entity e = scene.findByUuid(cubeId);
    ASSERT_TRUE(scene.registry().all_of<ScriptComponent>(e));
    EXPECT_FALSE(inspector->findChildren<editor::AddComponentMenu*>().isEmpty()) << "rebuilt";

    // A script file dropped on the object: the class it registers (named like the file first).
    const QString dir = app::toQString(editor->project().contentDir());
    QDir().mkpath(dir);
    const QString spinner = dir + QStringLiteral("/Spinner.cpp");
    {
        QFile f(spinner);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("// LUMORA_BEHAVIOUR(Commented);\n"
                "struct Helper : lumora::Behaviour {};\nLUMORA_BEHAVIOUR(Helper);\n"
                "struct Spinner : lumora::Behaviour {};\nLUMORA_BEHAVIOUR( Spinner );\n");
    }
    auto* hierarchy = editor->findChild<editor::SceneHierarchyPanel*>();
    ASSERT_NE(hierarchy, nullptr);
    scene.registry().get<ScriptComponent>(e).parameters = "speed=3";
    emit hierarchy->tree()->filesDropped(cubeId, {spinner});
    processEvents();
    e = scene.findByUuid(cubeId);
    EXPECT_EQ(scene.registry().get<ScriptComponent>(e).behaviour, "Spinner");
    EXPECT_EQ(scene.registry().get<ScriptComponent>(e).parameters, "") << "another class's parameters dropped";
    editor->context().undoStack().undo();
    e = scene.findByUuid(cubeId);
    EXPECT_EQ(scene.registry().get<ScriptComponent>(e).behaviour, "");

    // On the object under the cursor in the viewport; a file without a behaviour changes nothing.
    editor->viewportClient().camera().setPosition(glm::vec3(scene.worldMatrix(e)[3]) + glm::vec3(0.0f, 1.0f, 4.0f));
    editor->viewportClient().camera().lookAt(glm::vec3(scene.worldMatrix(e)[3]));
    editor->dropOnViewport({spinner}, glm::vec2(0.0f));
    processEvents();
    e = scene.findByUuid(cubeId);
    EXPECT_EQ(scene.registry().get<ScriptComponent>(e).behaviour, "Spinner");
    const QString plain = dir + QStringLiteral("/Util.hpp");
    {
        QFile f(plain);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("inline int twice(int x) { return 2 * x; }\n");
    }
    EXPECT_FALSE(editor->attachScript(cubeId, plain));
    EXPECT_EQ(scene.registry().get<ScriptComponent>(scene.findByUuid(cubeId)).behaviour, "Spinner");
}

#include "common/Gamepad.hpp"

TEST(Gamepad, DeadZoneIsRadialAndRescaled) {
    float x = 0.1f, y = 0.05f;
    app::GamepadReader::applyDeadZone(x, y, 0.15f);
    EXPECT_EQ(x, 0.0f);
    EXPECT_EQ(y, 0.0f);
    x = 1.0f;
    y = 0.0f;
    app::GamepadReader::applyDeadZone(x, y, 0.15f);
    EXPECT_FLOAT_EQ(x, 1.0f) << "still reaches the edge";
    x = 0.0f;
    y = -0.575f;  // half way between the dead zone and the edge
    app::GamepadReader::applyDeadZone(x, y, 0.15f);
    EXPECT_NEAR(y, -0.5f, 1e-5f);
    app::GamepadReader reader;  // no controller in CI: polling must be harmless
    InputState input;
    reader.poll(input);
    reader.poll(input);
    SUCCEED();
}

#include "editor/QuickOpen.hpp"

TEST(QuickOpen, FuzzyScorePrefersWordStartsAndNames) {
    EXPECT_LT(editor::fuzzyScore("xyz", "Content/Main.lscene"), 0);
    EXPECT_GE(editor::fuzzyScore("", "anything"), 0);
    EXPECT_GT(editor::fuzzyScore("main", "Content/Scenes/Main.lscene"),
              editor::fuzzyScore("main", "Content/Domains/Other.lscene")) << "a word start beats letters spread out";
    EXPECT_GT(editor::fuzzyScore("pc", "Scripts/PlayerController.cpp"), editor::fuzzyScore("pc", "Scripts/Epic.cpp"))
        << "CamelCase starts count";
    EXPECT_GT(editor::fuzzyScore("game", "Scripts/Game.cpp"), editor::fuzzyScore("game", "Game/Scripts/Other.cpp"))
        << "matches in the file name beat folders";
}

TEST_F(EditorUiTest, QuickOpenFindsFilesAndEntities) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    Scene& scene = *editor->editScene();
    entt::entity cube = entt::null;
    for (auto [e, rot] : scene.registry().view<RotatorComponent>().each()) cube = e;
    ASSERT_TRUE(cube != entt::null);
    const QString name = QString::fromStdString(scene.nameOf(cube));

    editor::QuickOpenDialog* dialog = editor->showQuickOpen();
    ASSERT_NE(dialog, nullptr);
    dialog->setQuery(QStringLiteral("main.lsc"));
    ASSERT_FALSE(dialog->shownLabels().isEmpty());
    EXPECT_EQ(dialog->shownLabels().front(), QStringLiteral("Content/Scenes/Main.lscene"));
    dialog->setQuery(name);
    ASSERT_FALSE(dialog->shownLabels().isEmpty());
    EXPECT_EQ(dialog->shownLabels().front(), name);
    dialog->openCurrent();
    EXPECT_EQ(editor->context().selectedUuid(), scene.uuidOf(cube)) << "Enter selects the entity";
}

#include "GltfTestUtils.hpp"
#include "editor/Thumbnails.hpp"
#include "renderer/Renderer.hpp"

#include <QImage>

TEST(Thumbnails, TypeIconsAndImagePreviewsCachedOnDisk) {
    EXPECT_EQ(editor::ThumbnailCache::typeLabel(QStringLiteral("cpp")), QStringLiteral("C++"));
    EXPECT_EQ(editor::ThumbnailCache::typeLabel(QStringLiteral("lscene")), QStringLiteral("SCENE"));
    EXPECT_EQ(editor::ThumbnailCache::typeLabel(QStringLiteral("wav")), QStringLiteral("WAV"));
    const QImage cpp = editor::ThumbnailCache::typeIcon(QStringLiteral("cpp")).pixmap(64, 64).toImage();
    const QImage wav = editor::ThumbnailCache::typeIcon(QStringLiteral("wav")).pixmap(64, 64).toImage();
    ASSERT_FALSE(cpp.isNull());
    EXPECT_NE(cpp, wav) << "each type has its own colour and label";

    test::TempDir project;
    const QString root = app::toQString(project.path());
    const QString png = root + QStringLiteral("/Content/red.png");
    QDir().mkpath(root + QStringLiteral("/Content"));
    QImage red(256, 128, QImage::Format_RGBA8888);
    red.fill(Qt::red);
    ASSERT_TRUE(red.save(png));
    editor::ThumbnailCache cache(root);
    QStringList ready;
    QObject::connect(&cache, &editor::ThumbnailCache::previewReady, [&](const QString& p) { ready << p; });
    (void)cache.icon(QFileInfo(png));  // the type icon now, the preview queued
    EXPECT_FALSE(cache.hasPreview(png));
    EXPECT_EQ(cache.pending(), 1);
    cache.processAll();
    EXPECT_EQ(ready, QStringList{png});
    const QImage preview = cache.icon(QFileInfo(png)).pixmap(128, 128).toImage();
    EXPECT_EQ(preview.pixelColor(preview.width() / 2, preview.height() / 2).red(), 255);
    EXPECT_FALSE(QDir(root + QStringLiteral("/Saved/Thumbnails")).entryList(QDir::Files).isEmpty()) << "kept on disk";
    editor::ThumbnailCache later(root);
    (void)later.icon(QFileInfo(png));
    EXPECT_TRUE(later.hasPreview(png)) << "the next session reads it from disk";
    EXPECT_EQ(later.pending(), 0);
}

TEST_F(EditorUiTest, PreviewsRenderAndChangedModelsReload) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    const auto models = editor->project().contentDir() / "Models";
    std::filesystem::create_directories(models);
    const auto cube = models / "cube.gltf";
    test::writeCubeGltf(cube, false);
    editor->reloadChangedIn(app::toQString(models));  // a new file: noted, nothing to reload
    auto first = editor->assets().model(cube);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    EXPECT_TRUE((*first)->images.empty()) << "the first version has no texture";

    if (context_->renderer() != nullptr) {
        const QImage preview = editor->renderPreview(app::toQString(cube));
        ASSERT_FALSE(preview.isNull());
        EXPECT_EQ(preview.size(), QSize(128, 128));
        const QColor c = preview.pixelColor(64, 64);
        EXPECT_GT(c.red(), c.green() + 30) << "the red cube fills the middle";
    }

    // Changed by another program: the next request reads the new file.
    test::writeCubeGltf(cube, false, true);
    std::filesystem::last_write_time(cube, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2));
    EXPECT_EQ(editor->reloadChangedIn(app::toQString(models)), 1);
    auto second = editor->assets().model(cube);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE((*second)->images.empty()) << "the new file has a texture";
    EXPECT_EQ(editor->reloadChangedIn(app::toQString(models)), 0) << "nothing new";
}

#include <QSettings>
#include <QTabWidget>

TEST_F(EditorUiTest, CodeEditorMovesIntoTheMainArea) {
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    auto* code = editor->findChild<editor::CodeEditorPanel*>();
    ASSERT_NE(code, nullptr);
    QTabWidget* center = qobject_cast<QTabWidget*>(editor->centralWidget());
    ASSERT_NE(center, nullptr);
    EXPECT_EQ(center->count(), 1) << "only the scene at first";
    editor->setCodeInMainArea(true);
    EXPECT_TRUE(editor->codeInMainArea());
    EXPECT_EQ(center->count(), 2);
    EXPECT_EQ(center->currentWidget(), code);
    editor->showScene();
    const auto script = editor->project().rootDir() / "Scripts" / "Note.cpp";
    std::filesystem::create_directories(script.parent_path());
    std::ofstream(script) << "// note\n";
    editor->openCode(app::toQString(script));
    EXPECT_EQ(center->currentWidget(), code) << "opening a file shows the code tab";
    editor->play();
    EXPECT_EQ(center->currentIndex(), 0) << "playing shows the scene";
    editor->stop();
    editor->setCodeInMainArea(false);
    EXPECT_EQ(center->count(), 1);
    auto* dock = qobject_cast<QDockWidget*>(code->parentWidget() ? code->parentWidget()->parentWidget() : nullptr);
    EXPECT_TRUE(dock != nullptr || code->window() == editor) << "back in its dock";
    EXPECT_EQ(code->window(), editor);
    QSettings().remove(QStringLiteral("editor/mainArea"));
}

#include "editor/GraphEditorPanel.hpp"

TEST_F(EditorUiTest, AnyPanelMovesIntoTheMainAreaAndStaysThere) {
    QSettings().remove(QStringLiteral("editor/mainArea"));
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    auto* center = qobject_cast<QTabWidget*>(editor->centralWidget());
    ASSERT_NE(center, nullptr);
    auto* graphDock = editor->findChild<QDockWidget*>(QStringLiteral("dock.graph"));
    auto* hierarchyDock = editor->findChild<QDockWidget*>(QStringLiteral("dock.hierarchy"));
    ASSERT_NE(graphDock, nullptr);
    ASSERT_NE(hierarchyDock, nullptr);
    auto* graph = editor->findChild<editor::GraphEditorPanel*>();
    ASSERT_NE(graph, nullptr);

    editor->setDockInMainArea(graphDock, true);
    editor->setDockInMainArea(hierarchyDock, true);
    EXPECT_EQ(center->count(), 3);
    EXPECT_EQ(center->tabText(1), QStringLiteral("Visual Script"));
    EXPECT_EQ(center->widget(1), graph);
    EXPECT_FALSE(graphDock->isVisible()) << "no empty dock left behind";
    EXPECT_FALSE(graphDock->toggleViewAction()->isEnabled());
    editor->showScene();
    editor->revealDock(graphDock);
    EXPECT_EQ(center->currentWidget(), graph) << "opening a graph shows its tab";

    // The placement survives a restart.
    application_->closeEditor();
    processEvents();
    editor = openEditor();
    ASSERT_NE(editor, nullptr);
    center = qobject_cast<QTabWidget*>(editor->centralWidget());
    ASSERT_EQ(center->count(), 3);
    EXPECT_EQ(center->tabText(1), QStringLiteral("Visual Script"));
    EXPECT_EQ(center->currentIndex(), 0) << "starts on the scene";

    // The tab's close button puts the panel back in its dock.
    graphDock = editor->findChild<QDockWidget*>(QStringLiteral("dock.graph"));
    emit center->tabCloseRequested(1);
    EXPECT_EQ(center->count(), 2);
    EXPECT_FALSE(editor->dockInMainArea(graphDock));
    EXPECT_EQ(graphDock->widget(), editor->findChild<editor::GraphEditorPanel*>());
    EXPECT_TRUE(graphDock->toggleViewAction()->isEnabled());
    QSettings().remove(QStringLiteral("editor/mainArea"));
}

TEST_F(EditorUiTest, ContentBrowserSnapshot) {
    // Visual check only: LUMORA_TEST_SNAPSHOT_DIR=... writes the browser with previews.
    const QString dir = qEnvironmentVariable("LUMORA_TEST_SNAPSHOT_DIR");
    if (dir.isEmpty()) GTEST_SKIP() << "set LUMORA_TEST_SNAPSHOT_DIR to write the picture";
    editor::EditorWindow* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    editor->resize(1600, 960);
    const auto content = editor->project().contentDir();
    std::filesystem::create_directories(content / "Mixed");
    test::writeCubeGltf(content / "Mixed" / "cube.gltf", false);
    MaterialAsset copper;
    copper.baseColor = {0.9f, 0.4f, 0.15f};
    copper.metallic = 1.0f;
    copper.roughness = 0.3f;
    ASSERT_TRUE(saveMaterial(copper, content / "Mixed" / "Copper.lmat").has_value());
    for (const char* name : {"Player.cpp", "Player.hpp", "notes.md", "Input.json", "jump.wav", "Level.lscene"}) {
        std::ofstream(content / "Mixed" / name) << "x";
    }
    QImage red(64, 64, QImage::Format_RGBA8888);
    red.fill(QColor(200, 60, 60));
    red.save(app::toQString(content / "Mixed" / "red.png"));
    auto* browser = editor->findChild<editor::ContentBrowserPanel*>();
    browser->navigate(app::toQString(content / "Mixed"));
    (void)waitFor([&] { return false; }, 1500);  // let the folder listing arrive
    for (int i = 0; i < 20; ++i) {
        editor->thumbnails().processAll();
        processEvents();
    }
    browser->grab().save(dir + QStringLiteral("/content_browser.png"));
}

#include "editor/AddComponentMenu.hpp"
#include "scene/ComponentRegistry.hpp"

#include <QLineEdit>
#include <QWidgetAction>

TEST(AddComponent, GroupsComponentsIntoCategoriesAndSearches) {
    using editor::AddComponentMenu;
    std::vector<AddComponentMenu::Entry> entries;
    for (const ComponentDesc& d : ComponentRegistry::builtin().components()) {
        if (d.userAddable) entries.push_back({QString::fromStdString(d.id), QString::fromStdString(d.label), QString::fromStdString(d.category)});
    }
    const auto count = static_cast<int>(entries.size());
    AddComponentMenu menu(std::move(entries));
    EXPECT_EQ(menu.filter({}), count) << "every component is in a category";
    QStringList submenus;
    for (QAction* a : menu.actions()) {
        if (a->menu() != nullptr) submenus << a->text();
    }
    EXPECT_TRUE(submenus.contains(QStringLiteral("Rendering")));
    EXPECT_TRUE(submenus.contains(QStringLiteral("Physics")));
    EXPECT_TRUE(submenus.contains(QStringLiteral("Environment")));
    EXPECT_EQ(submenus.indexOf(QStringLiteral("Core")), 0) << "Core first";
    for (QAction* a : menu.actions()) {
        if (a->menu() == nullptr || a->text() != QStringLiteral("Physics")) continue;
        QStringList names;
        for (QAction* item : a->menu()->actions()) names << item->text();
        EXPECT_TRUE(names.contains(QStringLiteral("Rigid Body")));
        EXPECT_TRUE(names.contains(QStringLiteral("Vehicle")));
        EXPECT_FALSE(names.contains(QStringLiteral("Light")));
    }

    QString chosen;
    QObject::connect(&menu, &AddComponentMenu::chosen, [&](const QString& id) { chosen = id; });
    menu.searchField()->setText(QStringLiteral("rigid"));
    QAction* first = nullptr;
    for (QAction* a : menu.actions()) {
        if (a->isSeparator() || a->menu() != nullptr || qobject_cast<QWidgetAction*>(a) != nullptr) continue;
        first = a;
        break;
    }
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(first->text().startsWith(QStringLiteral("Rigid Body")));
    emit menu.searchField()->returnPressed();
    processEvents();  // the choice is reported once the click is over
    EXPECT_EQ(chosen, QStringLiteral("RigidBody")) << "Enter adds the best match";
    EXPECT_GE(menu.filter(QStringLiteral("phys")), 5) << "a category name lists its components";
    EXPECT_EQ(menu.filter(QStringLiteral("zzzzqx")), 0);
}

#include "editor/TimelinePanel.hpp"
#include "scene/Timeline.hpp"

TEST(TimelineEditor, KeysPreviewsRestoresAndSaves) {
    test::TempDir dir;
    const QString file = QString::fromStdString((dir.path() / "Intro.ltimeline").string());
    ASSERT_TRUE(saveTimeline(TimelineAsset{}, dir.path() / "Intro.ltimeline").has_value());
    Scene scene;
    const entt::entity camera = scene.createEntity("Camera");
    auto& t = scene.registry().get<TransformComponent>(camera);
    editor::EditorContext ctx;
    ctx.setScene(&scene, false);
    ctx.select(scene.uuidOf(camera));
    editor::TimelinePanel panel(ctx, QString::fromStdString(dir.path().string()));
    ASSERT_TRUE(panel.open(file));

    panel.setTime(0.0f);
    ASSERT_TRUE(panel.keySelectedTransform());  // at the origin
    panel.setTime(2.0f);
    t.position = {8.0f, 0.0f, 0.0f};
    t.rotationDeg = {0.0f, 90.0f, 0.0f};
    ASSERT_TRUE(panel.keySelectedTransform());
    ASSERT_TRUE(panel.addEvent(QStringLiteral("door")));
    EXPECT_TRUE(panel.isDirty());
    ASSERT_EQ(panel.timeline().tracks.size(), 2u);
    EXPECT_EQ(panel.timeline().tracks[0].keys.size(), 2u);

    t.position = {1.0f, 2.0f, 3.0f};  // where the user left it
    panel.setTime(1.0f);
    panel.setPreview(true);
    EXPECT_NEAR(t.position.x, 4.0f, 0.3f) << "previewed halfway";
    panel.setTime(2.0f);
    EXPECT_NEAR(t.rotationDeg.y, 90.0f, 0.5f);
    panel.setPreview(false);
    EXPECT_EQ(t.position, glm::vec3(1.0f, 2.0f, 3.0f)) << "put back after the preview";

    ASSERT_TRUE(panel.save());
    EXPECT_FALSE(panel.isDirty());
    auto loaded = loadTimeline(dir.path() / "Intro.ltimeline");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->tracks[0].target, "Camera");
    EXPECT_FLOAT_EQ(loaded->tracks[0].keys[1].position.x, 8.0f);
    EXPECT_EQ(loaded->tracks[1].keys[0].text, "door");
}
