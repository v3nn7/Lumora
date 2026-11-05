#include "editor/EditorViewportClient.hpp"

#include "editor/EditorContext.hpp"
#include "scene/Picking.hpp"
#include "scene/Scene.hpp"

#include <QObject>

#include <glm/common.hpp>

#include <algorithm>

namespace lumora::editor {

EditorViewportClient::EditorViewportClient(EditorContext& ctx) : ctx_(ctx) {
    camera_.setPosition({0.0f, 3.0f, 8.0f});
    camera_.lookAt({0.0f, 0.5f, 0.0f});
}

bool EditorViewportClient::viewingThroughSceneCamera() const {
    return ctx_.isPlaying() && pilotSceneCamera && ctx_.scene() != nullptr &&
           ctx_.scene()->primaryCamera() != entt::null;
}

bool EditorViewportClient::gizmoVisible() const {
    return !ctx_.isPlaying() && ctx_.scene() != nullptr && ctx_.selectedEntity() != entt::null;
}

void EditorViewportClient::viewportTick(float dt, const InputState& input, bool) {
    if (!viewingThroughSceneCamera()) {
        camera_.update(input, dt);
    }
    if (onSimulate) onSimulate(dt);
    Scene* scene = ctx_.scene();
    if (scene != nullptr) scene->updateWorldTransforms();
    overlay_.clear();
    if (gizmoVisible()) {
        gizmo_.build(*scene, ctx_.selectedEntity(), camera_.view(lastAspect_), overlay_);
    }
}

bool EditorViewportClient::viewportMousePressed(glm::vec2 ndc, Qt::MouseButton button, Qt::KeyboardModifiers) {
    if (button != Qt::LeftButton || !gizmoVisible()) return false;
    if (!gizmo_.begin(*ctx_.scene(), ctx_.selectedEntity(), camera_.view(lastAspect_), ndc)) return false;
    dragTarget_ = ctx_.selectedUuid();
    return true;
}

void EditorViewportClient::viewportMouseMoved(glm::vec2 ndc, Qt::KeyboardModifiers modifiers) {
    Scene* scene = ctx_.scene();
    if (!gizmoVisible()) return;
    if (gizmo_.active()) {
        const entt::entity target = scene->findByUuid(dragTarget_);
        gizmo_.drag(*scene, target, camera_.view(lastAspect_), ndc, (modifiers & Qt::ControlModifier) != 0);
        ctx_.notifyValuesChanged(nullptr);  // live inspector values; recorded once on release
    } else {
        gizmo_.hover(*scene, ctx_.selectedEntity(), camera_.view(lastAspect_), ndc);
    }
}

void EditorViewportClient::viewportMouseReleased(glm::vec2, Qt::MouseButton button) {
    if (button != Qt::LeftButton || !gizmo_.active()) return;
    gizmo_.end();
    static const char* kNames[] = {"Move", "Rotate", "Scale"};
    ctx_.commitTransform(dragTarget_, QObject::tr(kNames[static_cast<int>(gizmo_.mode())]), gizmo_.startTransform());
    dragTarget_ = Uuid{};
}

const Scene* EditorViewportClient::viewportScene() const { return ctx_.scene(); }

CameraView EditorViewportClient::viewportCamera(float aspect) const {
    lastAspect_ = aspect;
    if (viewingThroughSceneCamera()) return sceneCameraView(*ctx_.scene(), aspect);
    return camera_.view(aspect);
}

render::RenderSettings EditorViewportClient::viewportSettings() const {
    render::RenderSettings s;
    s.showGrid = showGrid && !viewingThroughSceneCamera();
    s.selected = ctx_.isPlaying() ? entt::entity{entt::null} : ctx_.selectedEntity();
    s.overlay = overlay_.empty() ? nullptr : &overlay_;
    s.assets = assets;
    return s;
}

void EditorViewportClient::viewportClicked(glm::vec2 ndc) {
    const Scene* scene = ctx_.scene();
    if (scene == nullptr) return;
    const Ray ray = rayFromNdc(viewportCamera(lastAspect_), ndc);
    const entt::entity hit = pickEntity(*scene, ray, assets);
    ctx_.select(hit == entt::null ? Uuid{} : scene->uuidOf(hit));
}

void EditorViewportClient::frameEntity(entt::entity e) {
    const Scene* scene = ctx_.scene();
    if (scene == nullptr || e == entt::null) return;
    const glm::mat4 world = scene->worldMatrix(e);
    const glm::vec3 center(world[3]);
    const float size = std::max({glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                 glm::length(glm::vec3(world[2])), 0.5f});
    const float distance = std::clamp(size * 2.5f, 1.5f, 200.0f);
    camera_.setPosition(center - camera_.forward() * distance);
    camera_.lookAt(center);
}

void EditorViewportClient::frameSelection() { frameEntity(ctx_.selectedEntity()); }

bool EditorViewportClient::viewportKeyPressed(int key, Qt::KeyboardModifiers modifiers) {
    // While flying (RMB held) WASDQE belong to the camera.
    return onShortcut ? onShortcut(key, modifiers) : false;
}

} // namespace lumora::editor
