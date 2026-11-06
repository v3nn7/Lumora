#include "editor/EditorViewportClient.hpp"

#include "editor/EditorContext.hpp"
#include "scene/Picking.hpp"

#include <glm/gtx/euler_angles.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <algorithm>
#include "scene/Scene.hpp"

#include <QObject>

#include <glm/common.hpp>
#include <glm/matrix.hpp>

#include <algorithm>

namespace lumora::editor {

namespace {

/// Local transform from a matrix (no shear): translation, Euler YXZ degrees, scale.
void setTransformFromMatrix(TransformComponent& t, const glm::mat4& m) {
    t.position = glm::vec3(m[3]);
    const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    glm::mat4 rotation(1.0f);
    for (int c = 0; c < 3; ++c) rotation[c] = glm::vec4(glm::vec3(m[c]) / std::max(scale[c], 1e-8f), 0.0f);
    float y = 0.0f, x = 0.0f, z = 0.0f;
    glm::extractEulerAngleYXZ(rotation, y, x, z);
    t.rotationDeg = glm::degrees(glm::vec3(x, y, z));
    t.scale = scale;
}

} // namespace

EditorViewportClient::EditorViewportClient(EditorContext& ctx) : ctx_(ctx) {
    camera_.keyboardFlight = true;  // WASD/QE fly whenever the viewport has the keyboard
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
    if (scene != nullptr && !remoteMarkers.empty() && !viewingThroughSceneCamera()) drawRemoteMarkers(*scene);
    sprites_.clear();
    if (scene != nullptr && showIcons && !ctx_.isPlaying() && !viewingThroughSceneCamera()) {
        std::vector<entt::entity> selected;
        for (Uuid id : ctx_.selection()) selected.push_back(scene->findByUuid(id));
        icons_.build(*scene, camera_.view(lastAspect_).position, selected, pixelRatio, sprites_);
    }
    if (gizmoVisible()) {
        drawLightOf(*scene, ctx_.selectedEntity());
        drawColliderOf(*scene, ctx_.selectedEntity());
        gizmo_.build(*scene, ctx_.selectedEntity(), camera_.view(lastAspect_), overlay_);
    }
}

void EditorViewportClient::drawColliderOf(const Scene& scene, entt::entity e) {
    const auto* collider = scene.registry().try_get<ColliderComponent>(e);
    if (collider == nullptr) return;
    const CameraView view = camera_.view(lastAspect_);
    const glm::vec3 forward = -glm::normalize(glm::vec3(glm::inverse(view.view)[2]));
    render::OverlayBuilder b(overlay_, forward);
    const glm::mat4 world = scene.worldMatrix(e);
    glm::vec3 axes[3];
    glm::vec3 scale;
    for (int i = 0; i < 3; ++i) {
        scale[i] = glm::length(glm::vec3(world[i]));
        axes[i] = scale[i] > 1e-8f ? glm::vec3(world[i]) / scale[i] : glm::vec3(0.0f);
    }
    const glm::vec3 center = glm::vec3(world * glm::vec4(collider->offset, 1.0f));
    const float distance = glm::length(center - view.position);
    const float line = std::max(distance * 0.0012f, 0.002f);
    const glm::vec4 edge{0.35f, 0.95f, 0.45f, 0.9f};
    const glm::vec4 fill{0.35f, 0.95f, 0.45f, 0.08f};
    const glm::vec3 s = glm::abs(scale);
    switch (collider->shape) {
        case ColliderShape::Box: {
            const glm::vec3 half[3] = {axes[0] * collider->size.x * s.x * 0.5f, axes[1] * collider->size.y * s.y * 0.5f,
                                       axes[2] * collider->size.z * s.z * 0.5f};
            b.solidBox(center, half, fill);
            b.wireBox(center, half, line, edge);
            break;
        }
        case ColliderShape::Sphere: {
            const float r = collider->radius * std::max({s.x, s.y, s.z});
            for (const glm::vec3& axis : axes) b.torus(center, axis, r, line, edge, 48, 4);
            break;
        }
        case ColliderShape::Capsule: {
            const float r = collider->radius * std::max(s.x, s.z);
            const float h = collider->height * s.y * 0.5f;
            const glm::vec3 up = axes[1];
            for (const float side : {-1.0f, 1.0f}) {
                b.torus(center + up * (h * side), up, r, line, edge, 48, 4);
                // Hemisphere outlines: two half-rings approximated by full rings through the cap centre.
                b.torus(center + up * (h * side), axes[0], r, line, edge * glm::vec4(1, 1, 1, 0.5f), 48, 4);
            }
            for (const glm::vec3& side : {axes[0], -axes[0], axes[2], -axes[2]}) {
                b.cylinder(center + side * r - up * h, center + side * r + up * h, line, edge, 6);
            }
            break;
        }
    }
}

void EditorViewportClient::drawLightOf(const Scene& scene, entt::entity e) {
    const auto* light = scene.registry().try_get<LightComponent>(e);
    if (light == nullptr) return;
    const CameraView view = camera_.view(lastAspect_);
    const glm::vec3 forward = -glm::normalize(glm::vec3(glm::inverse(view.view)[2]));
    render::OverlayBuilder b(overlay_, forward);
    const glm::mat4 world = scene.worldMatrix(e);
    const glm::vec3 position(world[3]);
    const glm::vec3 axisX = glm::normalize(glm::vec3(world[0]));
    const glm::vec3 axisY = glm::normalize(glm::vec3(world[1]));
    const glm::vec3 dir = -glm::normalize(glm::vec3(world[2]));  // lights shine along local -Z
    const float line = std::max(glm::distance(view.position, position) * 0.0012f, 0.002f);
    const glm::vec4 color(glm::mix(light->color / std::max({light->color.r, light->color.g, light->color.b, 1e-4f}),
                                   glm::vec3(1.0f), 0.3f),
                          0.85f);
    switch (light->type) {
        case LightType::Point:
            for (const glm::vec3 axis : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)}) {
                b.torus(position, axis, light->range, line, color, 64, 4);
            }
            break;
        case LightType::Spot: {
            const float angle = glm::radians(std::clamp(light->spotAngleDeg, 1.0f, 89.0f));
            const glm::vec3 end = position + dir * light->range * std::cos(angle);
            const float radius = light->range * std::sin(angle);
            b.torus(end, dir, radius, line, color, 64, 4);
            for (const glm::vec3 side : {axisX, -axisX, axisY, -axisY}) b.cylinder(position, end + side * radius, line, color, 6);
            break;
        }
        case LightType::Directional: {
            // An arrow along the light's direction, a few metres long.
            const float length = std::max(glm::distance(view.position, position) * 0.15f, 0.5f);
            const glm::vec3 tip = position + dir * length;
            b.cylinder(position, tip - dir * (length * 0.2f), line * 1.5f, color, 8);
            b.cone(tip - dir * (length * 0.2f), tip, length * 0.06f, color);
            break;
        }
    }
}

void EditorViewportClient::drawRemoteMarkers(const Scene& scene) {
    const CameraView view = camera_.view(lastAspect_);
    const glm::vec3 eye(glm::inverse(view.view)[3]);
    const glm::vec3 viewForward = -glm::normalize(glm::vec3(glm::inverse(view.view)[2]));
    render::OverlayBuilder b(overlay_, viewForward);
    for (const RemoteMarker& m : remoteMarkers) {
        // What they have selected: a wire box around its bounds.
        if (const entt::entity e = scene.findByUuid(m.selection); e != entt::null) {
            const Aabb box = localBounds(scene, e, assets).value_or(Aabb{glm::vec3(-0.5f), glm::vec3(0.5f)});
            const glm::mat4 world = scene.worldMatrix(e);
            const glm::vec3 localCenter = (box.min + box.max) * 0.5f;
            const glm::vec3 half = (box.max - box.min) * 0.5f + glm::vec3(0.02f);
            const glm::vec3 center(world * glm::vec4(localCenter, 1.0f));
            const glm::vec3 axes[3] = {glm::vec3(world[0]) * half.x, glm::vec3(world[1]) * half.y,
                                       glm::vec3(world[2]) * half.z};
            const float line = std::max(glm::distance(eye, center) * 0.0015f, 0.003f);
            b.wireBox(center, axes, line, m.color);
        }
        // Their camera: a small body with a lens, sized to stay readable at any distance.
        if (!m.hasPose) continue;
        const float distance = glm::distance(eye, m.position);
        if (distance < 0.3f) continue;  // we are standing in it
        const float s = std::clamp(distance * 0.03f, 0.08f, 3.0f);
        const glm::vec3 f = glm::length(m.forward) > 1e-4f ? glm::normalize(m.forward) : glm::vec3(0, 0, -1);
        const glm::vec3 up0 = std::abs(f.y) > 0.98f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 r = glm::normalize(glm::cross(f, up0));
        const glm::vec3 u = glm::cross(r, f);
        const glm::vec3 body[3] = {r * (0.35f * s), u * (0.25f * s), f * (0.45f * s)};
        b.solidBox(m.position - f * (0.2f * s), body, m.color * glm::vec4(0.85f, 0.85f, 0.85f, 1.0f));
        b.cone(m.position + f * (0.9f * s), m.position + f * (0.2f * s), 0.35f * s, m.color * glm::vec4(1, 1, 1, 0.75f));
    }
}

bool EditorViewportClient::viewportMousePressed(glm::vec2 ndc, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    pressModifiers_ = modifiers;
    if (button != Qt::LeftButton || !gizmoVisible()) return false;
    Scene& scene = *ctx_.scene();
    if (!gizmo_.begin(scene, ctx_.selectedEntity(), camera_.view(lastAspect_), ndc)) return false;
    dragTarget_ = ctx_.selectedUuid();
    // The rest of the selection follows the primary as a group.
    groupStart_.clear();
    primaryWorldStart_ = scene.worldMatrix(ctx_.selectedEntity());
    for (entt::entity e : ctx_.selectedRoots()) {
        if (e == ctx_.selectedEntity()) continue;
        groupStart_.push_back({scene.uuidOf(e), scene.registry().get<TransformComponent>(e), scene.worldMatrix(e)});
    }
    return true;
}

void EditorViewportClient::moveGroup(Scene& scene) {
    if (groupStart_.empty()) return;
    const entt::entity primary = scene.findByUuid(dragTarget_);
    if (primary == entt::null) return;
    scene.updateWorldTransforms();
    const glm::mat4 delta = scene.worldMatrix(primary) * glm::inverse(primaryWorldStart_);
    for (const GroupMember& m : groupStart_) {
        const entt::entity e = scene.findByUuid(m.id);
        if (e == entt::null) continue;
        glm::mat4 local = delta * m.world;
        const entt::entity parent = scene.parentOf(e);
        if (parent != entt::null) local = glm::inverse(scene.worldMatrix(parent)) * local;
        setTransformFromMatrix(scene.registry().get<TransformComponent>(e), local);
    }
}

void EditorViewportClient::viewportMouseMoved(glm::vec2 ndc, Qt::KeyboardModifiers modifiers) {
    Scene* scene = ctx_.scene();
    if (!gizmoVisible()) return;
    if (gizmo_.active()) {
        const entt::entity target = scene->findByUuid(dragTarget_);
        gizmo_.drag(*scene, target, camera_.view(lastAspect_), ndc, (modifiers & Qt::ControlModifier) != 0);
        moveGroup(*scene);
        ctx_.notifyValuesChanged(nullptr);  // live inspector values; recorded once on release
    } else {
        gizmo_.hover(*scene, ctx_.selectedEntity(), camera_.view(lastAspect_), ndc);
    }
}

void EditorViewportClient::viewportMouseReleased(glm::vec2, Qt::MouseButton button) {
    if (button != Qt::LeftButton || !gizmo_.active()) return;
    gizmo_.end();
    static const char* kNames[] = {"Move", "Rotate", "Scale"};
    const QString text = QObject::tr(kNames[static_cast<int>(gizmo_.mode())]);
    if (groupStart_.empty()) {
        ctx_.commitTransform(dragTarget_, text, gizmo_.startTransform());
    } else {
        std::vector<std::pair<Uuid, TransformComponent>> before{{dragTarget_, gizmo_.startTransform()}};
        for (const GroupMember& m : groupStart_) before.emplace_back(m.id, m.transform);
        ctx_.commitTransforms(text, before);
    }
    groupStart_.clear();
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
    s.temporalAntiAliasing = true;  // a live view: consecutive frames
    s.showGrid = showGrid && !viewingThroughSceneCamera();
    s.selected = ctx_.isPlaying() ? entt::entity{entt::null} : ctx_.selectedEntity();
    if (!ctx_.isPlaying() && ctx_.scene() != nullptr && ctx_.selection().size() > 1) {
        for (Uuid id : ctx_.selection()) {
            const entt::entity e = ctx_.scene()->findByUuid(id);
            if (e != entt::null && e != s.selected) s.alsoSelected.push_back(e);
        }
    }
    s.overlay = overlay_.empty() ? nullptr : &overlay_;
    s.sprites = sprites_.empty() ? nullptr : &sprites_;
    s.assets = assets;
    s.hud = ctx_.isPlaying() ? hud : nullptr;
    s.debug = ctx_.isPlaying() ? debug : nullptr;
    return s;
}

Ray EditorViewportClient::rayAt(glm::vec2 ndc) const { return rayFromNdc(viewportCamera(lastAspect_), ndc); }

void EditorViewportClient::viewportClicked(glm::vec2 ndc) {
    const Scene* scene = ctx_.scene();
    // While playing, clicks belong to the game (its buttons); select in the Hierarchy instead.
    if (scene == nullptr || ctx_.isPlaying()) return;
    const Ray ray = rayFromNdc(viewportCamera(lastAspect_), ndc);
    entt::entity hit = entt::null;
    if (!sprites_.empty() && viewportPixels.x > 0.0f) {  // icons are on top, so they win
        const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * viewportPixels.x, (ndc.y * 0.5f + 0.5f) * viewportPixels.y);
        hit = SceneIcons::pick(sprites_, viewportCamera(lastAspect_), viewportPixels, pixel);
    }
    if (hit == entt::null) hit = pickEntity(*scene, ray, assets);
    if (pressModifiers_ & (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (hit != entt::null) ctx_.toggleSelected(scene->uuidOf(hit));  // add to / remove from the selection
        return;
    }
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
    return onShortcut ? onShortcut(key, modifiers) : false;
}

} // namespace lumora::editor
