#pragma once

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

#include <memory>

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lumora
