#include "editor/TransformGizmo.hpp"

#include "scene/Picking.hpp"
#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lumora::editor {

namespace {

constexpr float kPickTolerance = 0.045f;  // in units of half the view height

const glm::vec4 kAxisColors[3] = {{0.92f, 0.26f, 0.22f, 1.0f}, {0.42f, 0.82f, 0.25f, 1.0f}, {0.26f, 0.50f, 0.98f, 1.0f}};
const glm::vec4 kHighlight{1.0f, 0.82f, 0.18f, 1.0f};
const glm::vec4 kCenterColor{0.86f, 0.88f, 0.92f, 1.0f};

/// Projects to "screen units": NDC scaled by aspect on X so distances are isotropic.
std::optional<glm::vec2> toScreen(const CameraView& view, const glm::vec3& p) {
    const glm::vec4 clip = view.projection * view.view * glm::vec4(p, 1.0f);
    if (clip.w <= 1e-4f) return std::nullopt;
    return glm::vec2(clip.x / clip.w * view.aspect, clip.y / clip.w);
}

glm::vec2 mouseScreen(const CameraView& view, glm::vec2 ndc) { return {ndc.x * view.aspect, ndc.y}; }

float distanceToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

glm::vec3 perpendicular(const glm::vec3& n) {
    const glm::vec3 helper = std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    return glm::normalize(glm::cross(n, helper));
}

glm::quat rotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(r[i]);
        r[i] = len > 1e-8f ? r[i] / len : glm::vec3(0.0f);
    }
    return glm::normalize(glm::quat_cast(r));
}

glm::quat eulerToQuat(const glm::vec3& degrees) {
    const glm::vec3 r = glm::radians(degrees);
    return glm::normalize(glm::quat_cast(glm::eulerAngleYXZ(r.y, r.x, r.z)));
}

glm::vec3 quatToEuler(const glm::quat& q) {
    float y = 0.0f, x = 0.0f, z = 0.0f;
    glm::extractEulerAngleYXZ(glm::mat4_cast(q), y, x, z);
    return glm::degrees(glm::vec3(x, y, z));
}

float snapTo(float value, float step) { return std::round(value / step) * step; }

} // namespace

void TransformGizmo::setMode(GizmoMode mode) noexcept {
    if (active()) return;  // never switch mid-drag
    mode_ = mode;
    hovered_ = Handle::None;
}

TransformGizmo::Frame TransformGizmo::frameFor(const Scene& scene, entt::entity target, const CameraView& view) const {
    Frame f;
    const glm::mat4 world = scene.worldMatrix(target);
    f.origin = glm::vec3(world[3]);
    if (mode_ == GizmoMode::Scale) {
        for (int i = 0; i < 3; ++i) {
            const glm::vec3 axis(world[i]);
            const float len = glm::length(axis);
            f.axes[i] = len > 1e-8f ? axis / len : f.axes[i];
        }
    }
    // Constant on-screen size regardless of distance.
    const float distance = glm::length(f.origin - view.position);
    f.size = std::max(distance, view.nearPlane * 2.0f) * std::tan(view.fovYRadians * 0.5f) * kScreenSize;
    return f;
}

TransformGizmo::Handle TransformGizmo::pick(const Frame& f, const CameraView& view, glm::vec2 ndc) const {
    const glm::vec2 mouse = mouseScreen(view, ndc);
    const auto origin = toScreen(view, f.origin);
    if (!origin) return Handle::None;
    if (mode_ == GizmoMode::Scale && glm::length(mouse - *origin) < kPickTolerance * 1.3f) return Handle::Center;

    Handle best = Handle::None;
    float bestDistance = kPickTolerance;
    for (int i = 0; i < 3; ++i) {
        float distance = std::numeric_limits<float>::max();
        if (mode_ == GizmoMode::Rotate) {
            const glm::vec3 u = perpendicular(f.axes[i]);
            const glm::vec3 v = glm::cross(f.axes[i], u);
            std::optional<glm::vec2> previous;
            constexpr int kSamples = 64;
            for (int s = 0; s <= kSamples; ++s) {
                const float a = glm::two_pi<float>() * static_cast<float>(s) / kSamples;
                const auto p = toScreen(view, f.origin + (u * std::cos(a) + v * std::sin(a)) * f.size);
                if (p && previous) distance = std::min(distance, distanceToSegment(mouse, *previous, *p));
                previous = p;
            }
        } else {
            const auto tip = toScreen(view, f.origin + f.axes[i] * f.size);
            if (tip) distance = distanceToSegment(mouse, *origin, *tip);
        }
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<Handle>(static_cast<int>(Handle::X) + i);
        }
    }
    return best;
}

std::optional<float> TransformGizmo::axisParameter(const glm::vec3& origin, const glm::vec3& axis, const CameraView& view,
                                                   glm::vec2 ndc) {
    // Closest point between the axis line and the mouse ray.
    const Ray ray = rayFromNdc(view, ndc);
    const glm::vec3 w0 = origin - ray.origin;
    const float b = glm::dot(axis, ray.direction);
    const float d = glm::dot(axis, w0);
    const float e = glm::dot(ray.direction, w0);
    const float denom = 1.0f - b * b;
    if (denom < 1e-5f) return std::nullopt;  // looking straight down the axis
    return (b * e - d) / denom;
}

bool TransformGizmo::hover(const Scene& scene, entt::entity target, const CameraView& view, glm::vec2 ndc) {
    if (active()) return false;
    const Handle h = pick(frameFor(scene, target, view), view, ndc);
    const bool changed = h != hovered_;
    hovered_ = h;
    return changed;
}

bool TransformGizmo::begin(const Scene& scene, entt::entity target, const CameraView& view, glm::vec2 ndc) {
    const Frame f = frameFor(scene, target, view);
    const Handle h = pick(f, view, ndc);
    if (h == Handle::None) return false;
    active_ = h;
    hovered_ = h;
    dragFrame_ = f;
    start_ = scene.registry().get<TransformComponent>(target);
    const entt::entity parent = scene.parentOf(target);
    parentWorld_ = parent == entt::null ? glm::mat4(1.0f) : scene.worldMatrix(parent);
    startNdc_ = ndc;
    startParameter_ = 0.0f;
    startVectorValid_ = false;
    if (h != Handle::Center) {
        const glm::vec3 axis = f.axes[static_cast<int>(h) - static_cast<int>(Handle::X)];
        if (mode_ == GizmoMode::Rotate) {
            const Ray ray = rayFromNdc(view, ndc);
            const float denom = glm::dot(ray.direction, axis);
            if (std::abs(denom) > 1e-4f) {
                const float t = glm::dot(f.origin - ray.origin, axis) / denom;
                const glm::vec3 v = ray.origin + ray.direction * t - f.origin;
                if (t > 0.0f && glm::length(v) > 1e-6f) {
                    startVector_ = glm::normalize(v);
                    startVectorValid_ = true;
                }
            }
        } else if (auto t = axisParameter(f.origin, axis, view, ndc)) {
            startParameter_ = *t;
        }
    }
    return true;
}

void TransformGizmo::drag(Scene& scene, entt::entity target, const CameraView& view, glm::vec2 ndc, bool snap) {
    if (!active() || !scene.isValid(target)) return;
    TransformComponent& t = scene.registry().get<TransformComponent>(target);
    const Frame& f = dragFrame_;
    const int axisIndex = active_ == Handle::Center ? -1 : static_cast<int>(active_) - static_cast<int>(Handle::X);

    switch (mode_) {
        case GizmoMode::Translate: {
            const glm::vec3 axis = f.axes[axisIndex];
            const auto param = axisParameter(f.origin, axis, view, ndc);
            if (!param) return;
            float delta = *param - startParameter_;
            if (snap) delta = snapTo(delta, 0.25f);
            const glm::vec3 world = f.origin + axis * delta;
            t.position = glm::vec3(glm::inverse(parentWorld_) * glm::vec4(world, 1.0f));
            break;
        }
        case GizmoMode::Rotate: {
            if (!startVectorValid_) return;
            const glm::vec3 axis = f.axes[axisIndex];
            const Ray ray = rayFromNdc(view, ndc);
            const float denom = glm::dot(ray.direction, axis);
            if (std::abs(denom) < 1e-4f) return;
            const float hit = glm::dot(f.origin - ray.origin, axis) / denom;
            const glm::vec3 v = ray.origin + ray.direction * hit - f.origin;
            if (hit <= 0.0f || glm::length(v) < 1e-6f) return;
            const glm::vec3 current = glm::normalize(v);
            float angle = std::atan2(glm::dot(glm::cross(startVector_, current), axis), glm::dot(startVector_, current));
            if (snap) angle = snapTo(angle, glm::radians(15.0f));
            const glm::quat parentRotation = rotationOf(parentWorld_);
            const glm::quat local = glm::inverse(parentRotation) * glm::angleAxis(angle, axis) * parentRotation *
                                    eulerToQuat(start_.rotationDeg);
            t.rotationDeg = quatToEuler(glm::normalize(local));
            break;
        }
        case GizmoMode::Scale: {
            float factor = 1.0f;
            if (axisIndex < 0) {
                factor = 1.0f + (startNdc_.y - ndc.y) * 2.0f;  // drag up to grow
            } else {
                const auto param = axisParameter(f.origin, f.axes[axisIndex], view, ndc);
                if (!param) return;
                factor = (f.size + (*param - startParameter_)) / f.size;
            }
            if (snap) factor = snapTo(factor, 0.1f);
            factor = std::max(factor, 0.01f);
            t.scale = start_.scale;
            if (axisIndex < 0) {
                t.scale = start_.scale * factor;
            } else {
                t.scale[axisIndex] = start_.scale[axisIndex] * factor;
            }
            break;
        }
    }
    scene.updateWorldTransforms();
}

void TransformGizmo::end() noexcept { active_ = Handle::None; }

void TransformGizmo::build(const Scene& scene, entt::entity target, const CameraView& view,
                           render::OverlayGeometry& out) const {
    if (!scene.isValid(target)) return;
    const Frame f = active() ? Frame{frameFor(scene, target, view).origin,
                                     {dragFrame_.axes[0], dragFrame_.axes[1], dragFrame_.axes[2]},
                                     frameFor(scene, target, view).size}
                             : frameFor(scene, target, view);
    const glm::vec3 forward = -glm::normalize(glm::vec3(glm::inverse(view.view)[2]));
    render::OverlayBuilder b(out, forward);
    const float s = f.size;

    auto colorFor = [&](int i) {
        const Handle h = i < 0 ? Handle::Center : static_cast<Handle>(static_cast<int>(Handle::X) + i);
        glm::vec4 c = i < 0 ? kCenterColor : kAxisColors[i];
        if (h == active_ || (!active() && h == hovered_)) c = kHighlight;
        if (active() && h != active_) c.a = 0.25f;  // fade the other handles while dragging
        return c;
    };

    switch (mode_) {
        case GizmoMode::Translate:
            for (int i = 0; i < 3; ++i) {
                const glm::vec4 c = colorFor(i);
                b.cylinder(f.origin + f.axes[i] * (s * 0.12f), f.origin + f.axes[i] * (s * 0.76f), s * 0.024f, c);
                b.cone(f.origin + f.axes[i] * (s * 0.76f), f.origin + f.axes[i] * s, s * 0.07f, c);
            }
            b.box(f.origin, glm::vec3(s * 0.04f), kCenterColor);
            break;
        case GizmoMode::Rotate:
            for (int i = 0; i < 3; ++i) {
                const bool emphasised = static_cast<int>(active_) - 1 == i || static_cast<int>(hovered_) - 1 == i;
                b.torus(f.origin, f.axes[i], s, s * (emphasised ? 0.032f : 0.02f), colorFor(i));
            }
            b.box(f.origin, glm::vec3(s * 0.03f), kCenterColor);
            break;
        case GizmoMode::Scale:
            for (int i = 0; i < 3; ++i) {
                const glm::vec4 c = colorFor(i);
                b.cylinder(f.origin + f.axes[i] * (s * 0.12f), f.origin + f.axes[i] * (s * 0.84f), s * 0.022f, c);
                b.orientedBox(f.origin + f.axes[i] * (s * 0.9f), f.axes, s * 0.055f, c);
            }
            b.orientedBox(f.origin, f.axes, s * 0.07f, colorFor(-1));
            break;
    }
}

} // namespace lumora::editor
