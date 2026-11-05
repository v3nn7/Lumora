#pragma once

#include "assets/Primitives.hpp"
#include "core/Uuid.hpp"

#include <entt/entity/entity.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lumora {

/// Persistent identity. Every scene entity has exactly one.
struct IdComponent {
    Uuid id;
};

struct NameComponent {
    std::string name;
};

/// Local transform relative to the parent. Rotation is stored as Euler angles in degrees
/// (applied yaw(Y) * pitch(X) * roll(Z)), which keeps editing and serialization lossless.
struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotationDeg{0.0f};
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 localMatrix() const;
};

/// Cached world matrix, refreshed by Scene::updateWorldTransforms().
struct WorldTransformComponent {
    glm::mat4 matrix{1.0f};
};

struct HierarchyComponent {
    entt::entity parent{entt::null};
    std::vector<entt::entity> children;
};

struct MeshRendererComponent {
    MeshPrimitive mesh = MeshPrimitive::Cube;
    glm::vec3 baseColor{0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive{0.0f};
    float emissiveStrength = 0.0f;
    bool visible = true;
};

enum class LightType : std::int32_t { Directional = 0, Point = 1 };

/// Directional lights shine along the entity's local -Z axis; intensity in lux-like units.
struct LightComponent {
    LightType type = LightType::Directional;
    glm::vec3 color{1.0f};
    float intensity = 3.0f;
    float range = 10.0f;
};

struct CameraComponent {
    float fovDeg = 60.0f;
    float nearPlane = 0.05f;
    bool primary = true;
};

/// Built-in gameplay behaviour: spins the entity while the simulation runs.
struct RotatorComponent {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float speedDeg = 45.0f;
};

/// Scene-wide settings stored alongside entities.
struct EnvironmentSettings {
    glm::vec3 skyZenithColor{0.11f, 0.26f, 0.62f};
    glm::vec3 skyHorizonColor{0.52f, 0.64f, 0.80f};
    glm::vec3 groundColor{0.16f, 0.14f, 0.12f};
    float ambientIntensity = 0.5f;
    float exposure = 0.9f;
};

} // namespace lumora
