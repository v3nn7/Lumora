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

/// Procedural world-space surface pattern multiplied into the base colour (prototyping material).
enum class SurfacePattern : std::int32_t { None = 0, Grid = 1, Checker = 2 };

struct MeshRendererComponent {
    MeshPrimitive mesh = MeshPrimitive::Cube;
    /// Imported mesh ("Content/Models/X.glb#0"); overrides `mesh` when set.
    std::string meshAsset;
    glm::vec3 baseColor{0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive{0.0f};
    float emissiveStrength = 0.0f;
    SurfacePattern pattern = SurfacePattern::None;
    // Texture maps (project-relative keys, see AssetLibrary). Empty = none.
    std::string baseColorTexture;
    std::string metallicRoughnessTexture;  // glTF packing: G = roughness, B = metallic
    std::string normalTexture;
    std::string emissiveTexture;
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

/// Runs a C++ behaviour from the project's game library (Scripts/) while the simulation runs.
struct ScriptComponent {
    std::string behaviour;   // class name registered with LUMORA_BEHAVIOUR
    std::string parameters;  // "speed=4, jump=6": numbers the behaviour reads with param()
};

/// Plays a sound file (project-relative WAV, FLAC or MP3) while the simulation runs, in 3D at the
/// entity (spatial) or flat. Scripts start and stop it with playSound() / stopSound().
struct AudioSourceComponent {
    std::string clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool playOnStart = true;
    bool spatial = true;
    float minDistance = 1.0f;   // full volume up to here (metres)
    float maxDistance = 50.0f;  // no further attenuation beyond
};

/// Plays a glTF model's animation clip on the entities instantiated from that model (the
/// entity carrying this is the model's root; its descendants carry ModelNodeComponent).
struct AnimatorComponent {
    std::string model;         // project-relative .glb/.gltf the clips come from
    std::string clip;          // clip name; empty = the model's first clip
    float speed = 1.0f;
    bool loop = true;
    bool playOnStart = true;
};

/// The mesh on this entity is deformed by a skin of its model: the joints are the entities
/// mirroring the skin's joint nodes (ModelNodeComponent) under the same model root.
struct SkinComponent {
    int skin = -1;  // index into the model's skins (the model is the one of MeshRenderer::meshAsset)
};

/// Which node of the source model an entity mirrors (set by instantiateModel; used by animation).
struct ModelNodeComponent {
    int node = -1;
};

/// Built-in gameplay behaviour: spins the entity while the simulation runs.
struct RotatorComponent {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float speedDeg = 45.0f;
};

enum class BodyMotion : std::int32_t { Static = 0, Kinematic = 1, Dynamic = 2 };

/// Simulated body. Without a RigidBody, an entity with a Collider is a static obstacle.
/// Kinematic bodies follow the entity transform (e.g. animated by a Rotator) and push dynamic ones.
struct RigidBodyComponent {
    BodyMotion motion = BodyMotion::Dynamic;
    float mass = 1.0f;  // kg; inertia is derived from the collider shape
    float friction = 0.5f;
    float restitution = 0.1f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    float gravityFactor = 1.0f;
};

enum class ColliderShape : std::int32_t { Box = 0, Sphere = 1, Capsule = 2 };

/// Collision shape in the entity's local space; the entity's world scale is applied.
struct ColliderComponent {
    ColliderShape shape = ColliderShape::Box;
    glm::vec3 size{1.0f};    // box full extents
    float radius = 0.5f;     // sphere / capsule
    float height = 1.0f;     // capsule cylinder part (total = height + 2 * radius)
    glm::vec3 offset{0.0f};  // local-space centre offset
};

/// Scene-wide settings stored alongside entities.
struct EnvironmentSettings {
    glm::vec3 skyZenithColor{0.07f, 0.20f, 0.56f};
    glm::vec3 skyHorizonColor{0.56f, 0.67f, 0.82f};
    glm::vec3 groundColor{0.16f, 0.14f, 0.12f};
    float ambientIntensity = 0.5f;
    float exposure = 0.9f;
    float fogDensity = 0.008f;        // extinction per metre at height 0
    float fogHeightFalloff = 0.12f;   // fog thins out with altitude
    float bloomIntensity = 0.05f;     // fraction of the blurred image mixed back in
    float vignette = 0.35f;
    // Image-based lighting: an equirectangular .hdr replaces the procedural sky and lights the
    // scene (reflections and ambient). Empty = procedural sky.
    std::string skyTexture;
    float skyIntensity = 1.0f;
    float skyRotationDeg = 0.0f;  // turns the environment around the vertical axis
};

} // namespace lumora
