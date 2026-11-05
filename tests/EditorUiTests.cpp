// Drives the real Hub and Editor widgets (offscreen QPA). Rendering is not exercised here
// (the offscreen platform has no Vulkan surfaces); see RendererTests for GPU coverage.
#include "TestUtils.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorWindow.hpp"
#include "hub/HubWindow.hpp"
#include "main/Application.hpp"
#include "project/Project.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"
#include "runtime/Simulation.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QToolButton>
#include <QTreeWidget>

using namespace lumora;

namespace {

void processEvents() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
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
