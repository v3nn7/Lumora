// Transform gizmo math (pure, no GPU) and an offscreen render of the overlay pass.
#include "editor/TransformGizmo.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Scene.hpp"

#include <QImage>
#include <QString>

#include <gtest/gtest.h>

using namespace lumora;
using namespace lumora::editor;

namespace {

struct GizmoFixture : ::testing::Test {
    GizmoFixture() {
        target = scene.createEntity("Target");
        scene.registry().emplace<MeshRendererComponent>(target);
        scene.updateWorldTransforms();
        camera.setPosition({2.0f, 3.0f, 8.0f});
        camera.lookAt({0.0f, 0.0f, 0.0f});
        view = camera.view(16.0f / 9.0f);
    }

    glm::vec2 ndcOf(const glm::vec3& world) const {
        const glm::vec4 clip = view.projection * view.view * glm::vec4(world, 1.0f);
        return glm::vec2(clip) / clip.w;
    }
    float gizmoSize() const {
        return glm::length(view.position) * std::tan(view.fovYRadians * 0.5f) * TransformGizmo::kScreenSize;
    }

    Scene scene;
    entt::entity target = entt::null;
    FlyCameraController camera;
    CameraView view;
    TransformGizmo gizmo;
};

} // namespace

TEST_F(GizmoFixture, TranslateAlongXAxis) {
    const float s = gizmoSize();
    ASSERT_TRUE(gizmo.begin(scene, target, view, ndcOf({s * 0.5f, 0, 0}))) << "grab the X shaft";
    gizmo.drag(scene, target, view, ndcOf({s * 0.5f + 2.0f, 0, 0}), false);
    const auto& t = scene.registry().get<TransformComponent>(target);
    EXPECT_NEAR(t.position.x, 2.0f, 1e-2f);
    EXPECT_NEAR(t.position.y, 0.0f, 1e-4f);
    EXPECT_NEAR(t.position.z, 0.0f, 1e-4f);
    gizmo.drag(scene, target, view, ndcOf({s * 0.5f + 1.13f, 0, 0}), true);
    EXPECT_NEAR(scene.registry().get<TransformComponent>(target).position.x, 1.25f, 1e-4f) << "Ctrl snaps to 0.25";
    gizmo.end();
    EXPECT_FALSE(gizmo.active());
}

TEST_F(GizmoFixture, MissingTheHandlesDoesNotStartADrag) {
    EXPECT_FALSE(gizmo.begin(scene, target, view, glm::vec2(0.9f, 0.9f)));
}

TEST_F(GizmoFixture, TranslateRespectsParentSpace) {
    const entt::entity parent = scene.createEntity("Parent");
    scene.registry().get<TransformComponent>(parent).scale = glm::vec3(2.0f);
    scene.setParent(target, parent);
    scene.updateWorldTransforms();
    const float s = gizmoSize();
    ASSERT_TRUE(gizmo.begin(scene, target, view, ndcOf({s * 0.5f, 0, 0})));
    gizmo.drag(scene, target, view, ndcOf({s * 0.5f + 2.0f, 0, 0}), false);
    // Two world units under a 2x-scaled parent is one local unit.
    EXPECT_NEAR(scene.registry().get<TransformComponent>(target).position.x, 1.0f, 1e-2f);
    EXPECT_NEAR(scene.worldMatrix(target)[3].x, 2.0f, 2e-2f);
}

TEST_F(GizmoFixture, RotateAboutYAxis) {
    gizmo.setMode(GizmoMode::Rotate);
    const float s = gizmoSize();
    // The Y ring lies in the XZ plane; grab it at +X and drag a quarter turn to -Z.
    ASSERT_TRUE(gizmo.begin(scene, target, view, ndcOf({s, 0, 0})));
    gizmo.drag(scene, target, view, ndcOf({0, 0, -s}), false);
    const glm::vec3 r = scene.registry().get<TransformComponent>(target).rotationDeg;
    EXPECT_NEAR(r.y, 90.0f, 0.5f);
    EXPECT_NEAR(r.x, 0.0f, 0.5f);
    EXPECT_NEAR(r.z, 0.0f, 0.5f);
}

TEST_F(GizmoFixture, ScaleAlongLocalAxis) {
    gizmo.setMode(GizmoMode::Scale);
    const float s = gizmoSize();
    ASSERT_TRUE(gizmo.begin(scene, target, view, ndcOf({s * 0.6f, 0, 0})));
    gizmo.drag(scene, target, view, ndcOf({s * 0.6f + s, 0, 0}), false);
    const glm::vec3 scale = scene.registry().get<TransformComponent>(target).scale;
    EXPECT_NEAR(scale.x, 2.0f, 2e-2f) << "dragging one gizmo length doubles the scale";
    EXPECT_FLOAT_EQ(scale.y, 1.0f);
    EXPECT_FLOAT_EQ(scale.z, 1.0f);
}

TEST_F(GizmoFixture, OverlayRendersOnTopOfTheScene) {
    render::RenderContextConfig config;
    config.requirePresentation = false;
    std::unique_ptr<render::RenderContext> context;
    try {
        context = std::make_unique<render::RenderContext>(config);
    } catch (const rhi::UnsupportedHardwareError& e) {
        GTEST_SKIP() << "no suitable Vulkan 1.4 GPU: " << e.what();
    }
    render::Renderer renderer(*context);
    // An entity sits in front of the gizmo origin (between it and the camera).
    const entt::entity blocker = scene.createEntity("Blocker");
    scene.registry().emplace<MeshRendererComponent>(blocker).baseColor = glm::vec3(0.05f);
    scene.registry().get<TransformComponent>(blocker).position = glm::vec3(view.position) * 0.5f;
    scene.updateWorldTransforms();

    render::OverlayGeometry overlay;
    gizmo.build(scene, target, view, overlay);
    ASSERT_FALSE(overlay.empty());
    render::RenderSettings settings;
    settings.overlay = &overlay;
    constexpr std::uint32_t kW = 1280, kH = 720;
    auto image = renderer.capture(scene, view, {kW, kH}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;

    // The X arrow head must be visible (red) despite the occluder in front of it.
    const float s = gizmoSize();
    const glm::vec2 ndc = ndcOf({s * 0.84f, 0, 0});
    const int px = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(kW));
    const int py = static_cast<int>((ndc.y * 0.5f + 0.5f) * static_cast<float>(kH));
    const std::size_t i = (static_cast<std::size_t>(py) * kW + static_cast<std::size_t>(px)) * 4;
    EXPECT_GT(image->rgba[i], 150) << "red channel";
    EXPECT_LT(image->rgba[i + 1], 110) << "green channel";
    EXPECT_LT(image->rgba[i + 2], 110) << "blue channel";

    if (const QString dir = qEnvironmentVariable("LUMORA_TEST_SNAPSHOT_DIR"); !dir.isEmpty()) {
        auto big = renderer.capture(scene, camera.view(16.0f / 9.0f), {960, 540}, settings);
        if (big) {
            QImage(big->rgba.data(), 960, 540, QImage::Format_RGBA8888).save(dir + QStringLiteral("/gizmo.png"));
        }
    }
}

#include "editor/EditorContext.hpp"
#include "editor/EditorViewportClient.hpp"

TEST(MultiSelection, GizmoMovesTheGroupAndEditsApplyToAll) {
    Scene scene;
    const entt::entity a = scene.createEntity("A");
    const entt::entity b = scene.createEntity("B");
    const entt::entity child = scene.createEntity("Child", Uuid::generate(), b);
    for (entt::entity e : {a, b, child}) scene.registry().emplace<MeshRendererComponent>(e);
    scene.registry().get<TransformComponent>(b).position = {3.0f, 0.0f, 0.0f};
    scene.registry().get<TransformComponent>(child).position = {0.0f, 1.0f, 0.0f};
    scene.updateWorldTransforms();
    EditorContext ctx;
    ctx.setScene(&scene, false);
    const Uuid ia = scene.uuidOf(a), ib = scene.uuidOf(b), ic = scene.uuidOf(child);

    ctx.setSelection({ib, ic, ia});  // A primary (last)
    EXPECT_EQ(ctx.selectedUuid(), ia);
    EXPECT_EQ(ctx.selectedRoots().size(), 2u) << "the child moves with its selected parent";
    ctx.toggleSelected(ic);
    EXPECT_FALSE(ctx.isSelected(ic));
    EXPECT_EQ(ctx.selectedUuid(), ia);

    // Drag A's X arrow by 2 m: B follows, its child with it.
    EditorViewportClient client(ctx);
    client.camera().setPosition({2.0f, 3.0f, 8.0f});
    client.camera().lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = client.viewportCamera(16.0f / 9.0f);
    auto ndcOf = [&](const glm::vec3& world) {
        const glm::vec4 clip = view.projection * view.view * glm::vec4(world, 1.0f);
        return glm::vec2(clip) / clip.w;
    };
    const float s = glm::length(view.position) * std::tan(view.fovYRadians * 0.5f) * TransformGizmo::kScreenSize;
    ASSERT_TRUE(client.viewportMousePressed(ndcOf({s * 0.5f, 0, 0}), Qt::LeftButton, Qt::NoModifier));
    client.viewportMouseMoved(ndcOf({s * 0.5f + 2.0f, 0, 0}), Qt::NoModifier);
    client.viewportMouseReleased(ndcOf({s * 0.5f + 2.0f, 0, 0}), Qt::LeftButton);
    EXPECT_NEAR(scene.registry().get<TransformComponent>(a).position.x, 2.0f, 1e-2f);
    EXPECT_NEAR(scene.registry().get<TransformComponent>(b).position.x, 5.0f, 1e-2f) << "moved with the primary";
    EXPECT_NEAR(scene.registry().get<TransformComponent>(child).position.y, 1.0f, 1e-4f) << "local, unchanged";
    ctx.undoStack().undo();
    EXPECT_NEAR(scene.registry().get<TransformComponent>(a).position.x, 0.0f, 1e-4f);
    EXPECT_NEAR(scene.registry().get<TransformComponent>(b).position.x, 3.0f, 1e-4f) << "one undo step for the group";

    // One field on all selected: one undo step.
    const ComponentDesc* mesh = ComponentRegistry::builtin().find("MeshRenderer");
    ASSERT_NE(mesh, nullptr);
    const FieldDesc* color = mesh->findField("baseColor");
    ctx.setFieldOnSelection(*mesh, *color, FieldValue(glm::vec3(1.0f, 0.0f, 0.0f)));
    EXPECT_EQ(scene.registry().get<MeshRendererComponent>(a).baseColor, glm::vec3(1, 0, 0));
    EXPECT_EQ(scene.registry().get<MeshRendererComponent>(b).baseColor, glm::vec3(1, 0, 0));
    EXPECT_NE(scene.registry().get<MeshRendererComponent>(child).baseColor, glm::vec3(1, 0, 0)) << "not selected";
    ctx.undoStack().undo();
    EXPECT_EQ(scene.registry().get<MeshRendererComponent>(b).baseColor, glm::vec3(0.8f));

    // Duplicate and delete work on the whole selection.
    ctx.duplicateSelected();
    EXPECT_EQ(scene.entityCount(), 6u) << "A and B (with its child) copied";
    EXPECT_EQ(ctx.selection().size(), 2u) << "the copies are selected";
    ctx.deleteSelected();
    EXPECT_EQ(scene.entityCount(), 3u);
    EXPECT_TRUE(ctx.selection().empty());
    ctx.undoStack().undo();
    EXPECT_EQ(scene.entityCount(), 6u) << "deleting both was one step";
}
