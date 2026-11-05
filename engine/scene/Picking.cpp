#include "scene/Picking.hpp"

#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace lumora {

namespace {
const std::array<Aabb, static_cast<std::size_t>(MeshPrimitive::Count)>& primitiveBounds() {
    static const auto bounds = [] {
        std::array<Aabb, static_cast<std::size_t>(MeshPrimitive::Count)> b{};
        for (std::size_t i = 0; i < b.size(); ++i) {
            b[i] = makePrimitive(static_cast<MeshPrimitive>(i)).bounds;
        }
        return b;
    }();
    return bounds;
}
} // namespace

Ray rayFromNdc(const CameraView& view, glm::vec2 ndc) {
    const glm::mat4 invViewProj = glm::inverse(view.projection * view.view);
    // Reverse-Z: depth 1 is the near plane, a small positive depth is far away.
    glm::vec4 nearPoint = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);
    glm::vec4 farPoint = invViewProj * glm::vec4(ndc, 0.001f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    return {glm::vec3(nearPoint), glm::normalize(glm::vec3(farPoint - nearPoint))};
}

std::optional<float> intersectRayAabb(const Ray& ray, const Aabb& box) {
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        const float o = ray.origin[axis];
        const float d = ray.direction[axis];
        if (std::abs(d) < 1e-8f) {
            if (o < box.min[axis] || o > box.max[axis]) return std::nullopt;
            continue;
        }
        float t0 = (box.min[axis] - o) / d;
        float t1 = (box.max[axis] - o) / d;
        if (t0 > t1) std::swap(t0, t1);
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) return std::nullopt;
    }
    return tMin;
}

entt::entity pickEntity(const Scene& scene, const Ray& ray) {
    entt::entity best = entt::null;
    float bestDistance = std::numeric_limits<float>::max();
    const auto& reg = scene.registry();
    for (auto [e, mesh, world] : reg.view<MeshRendererComponent, WorldTransformComponent>().each()) {
        if (!mesh.visible) continue;
        const glm::mat4 inv = glm::inverse(world.matrix);
        Ray local{glm::vec3(inv * glm::vec4(ray.origin, 1.0f)), glm::vec3(inv * glm::vec4(ray.direction, 0.0f))};
        const float scale = glm::length(local.direction);
        if (scale < 1e-12f) continue;
        local.direction /= scale;
        Aabb box = primitiveBounds()[static_cast<std::size_t>(mesh.mesh)];
        // Give flat meshes a little thickness so planes remain clickable edge-on.
        box.min -= glm::vec3(1e-3f);
        box.max += glm::vec3(1e-3f);
        if (auto t = intersectRayAabb(local, box)) {
            const glm::vec3 hitWorld = glm::vec3(world.matrix * glm::vec4(local.origin + local.direction * *t, 1.0f));
            const float distance = glm::length(hitWorld - ray.origin);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = e;
            }
        }
    }
    return best;
}

} // namespace lumora
