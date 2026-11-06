#pragma once

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

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
/// entity's world scale at creation time.
class PhysicsWorld {
public:
    explicit PhysicsWorld(Scene& scene);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    /// True if the scene has anything to simulate.
    [[nodiscard]] static bool sceneHasColliders(const Scene& scene);

    void step(float dt);

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
    /// Pairs of entities whose bodies started touching since the last call (each pair once).
    [[nodiscard]] std::vector<std::pair<entt::entity, entt::entity>> takeContacts();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lumora
