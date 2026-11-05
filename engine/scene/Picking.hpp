#pragma once

#include "assets/MeshData.hpp"
#include "scene/Camera.hpp"

#include <entt/entity/entity.hpp>
#include <glm/vec2.hpp>

#include <optional>

namespace lumora {

class Scene;

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

/// Ray through a viewport pixel. ndc is in [-1, 1] with +Y down (Vulkan convention).
[[nodiscard]] Ray rayFromNdc(const CameraView& view, glm::vec2 ndc);
/// Distance along the ray to the box, if hit (slab test).
[[nodiscard]] std::optional<float> intersectRayAabb(const Ray& ray, const Aabb& box);
/// Closest visible mesh entity under the ray, tested against its local-space bounds.
[[nodiscard]] entt::entity pickEntity(const Scene& scene, const Ray& ray);

} // namespace lumora
