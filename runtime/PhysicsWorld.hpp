#pragma once

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lumora {

class Scene;

/// Rigid-body simulation of a scene (Jolt Physics).
///
/// Entities with a ColliderComponent become bodies: static without a RigidBodyComponent,
/// otherwise static/kinematic/dynamic as configured. Every step: bodies are added/removed to
/// match the scene, kinematic bodies move to their entity's transform, the world advances and
/// dynamic bodies write their pose back (converted into parent space). Shapes include the
/// entity's world scale at creation time. Terrains are static triangle meshes; Water makes
/// dynamic bodies float.
class PhysicsWorld {
public:
    explicit PhysicsWorld(Scene& scene);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    /// True if the scene has anything to simulate.
    [[nodiscard]] static bool sceneHasColliders(const Scene& scene);

    void step(float dt);
    /// Project folder terrain heightmaps are read from.
    void setProjectRoot(std::filesystem::path root);
    [[nodiscard]] std::size_t terrainCount() const;

    [[nodiscard]] std::size_t bodyCount() const;
    void setGravity(const glm::vec3& gravity);
    /// Linear velocity of a dynamic body (zero if the entity has none).
    [[nodiscard]] glm::vec3 linearVelocity(entt::entity entity) const;
    /// Sets the velocity of a dynamic body. False if the entity has no dynamic body.
    bool setLinearVelocity(entt::entity entity, const glm::vec3& velocity);
    /// Force (N) applied during the next step, or an instant impulse (N s). Dynamic bodies only.
    bool addForce(entt::entity entity, const glm::vec3& force, bool impulse);

    struct RayHit {
        entt::entity entity = entt::null;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        float distance = 0.0f;
    };
    /// Closest body hit by a ray (direction need not be normalised), within maxDistance.
    [[nodiscard]] std::optional<RayHit> raycast(const glm::vec3& origin, const glm::vec3& direction,
                                                float maxDistance) const;
    // Characters (CharacterControllerComponent).
    /// Desired walking velocity (m/s, world space; the vertical part is ignored: gravity and jumps own it).
    bool moveCharacter(entt::entity entity, const glm::vec3& velocity);
    /// Jumps with this upward speed (m/s) if the character stands on the ground.
    bool jumpCharacter(entt::entity entity, float speed);
    [[nodiscard]] bool characterGrounded(entt::entity entity) const;
    [[nodiscard]] std::size_t characterCount() const;

    // Joints, vehicles, ragdolls.
    [[nodiscard]] std::size_t jointCount() const;
    [[nodiscard]] std::size_t vehicleCount() const;
    /// Bodies of all active ragdolls.
    [[nodiscard]] std::size_t ragdollBodyCount() const;
    /// Driver input of a VehicleComponent: throttle -1..1 (negative = reverse), steering -1..1
    /// (positive = right), brake and handbrake 0..1.
    bool driveVehicle(entt::entity entity, float throttle, float steering, float brake, float handbrake);
    /// Forward speed of a vehicle in m/s (negative when reversing).
    [[nodiscard]] float vehicleSpeed(entt::entity entity) const;

    /// Pairs of entities whose bodies started touching since the last call (each pair once).
    [[nodiscard]] std::vector<std::pair<entt::entity, entt::entity>> takeContacts();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lumora
