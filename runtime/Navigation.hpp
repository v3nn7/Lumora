#pragma once

#include "core/Error.hpp"

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace lumora {

class PhysicsWorld;
class Scene;

struct NavMeshSettings {
    float cellSize = 0.2f;      // metres per voxel horizontally
    float cellHeight = 0.1f;
    float agentRadius = 0.4f;   // the mesh keeps this far from walls
    float agentHeight = 1.8f;   // ceilings lower than this block the way
    float maxClimb = 0.4f;      // steps up to this high are walkable
    float maxSlopeDeg = 45.0f;
};

/// Navigation for a scene (Recast & Detour): a navigation mesh built from the static colliders
/// (what characters can stand on), path queries, and a crowd that walks the Nav Agents to their
/// targets around obstacles and each other.
class NavigationWorld {
public:
    explicit NavigationWorld(Scene& scene, NavMeshSettings settings = {});
    ~NavigationWorld();
    NavigationWorld(const NavigationWorld&) = delete;
    NavigationWorld& operator=(const NavigationWorld&) = delete;

    /// Project folder terrain heightmaps are read from.
    void setProjectRoot(std::filesystem::path root);
    /// (Re)builds the navigation mesh from the scene's static colliders and terrains now.
    Result<> build();
    [[nodiscard]] bool built() const noexcept;
    [[nodiscard]] std::size_t polygonCount() const;

    /// Corner points of the shortest walkable way (start and end included); empty if there is none.
    [[nodiscard]] std::vector<glm::vec3> findPath(const glm::vec3& from, const glm::vec3& to);
    /// Nearest point on the mesh within a few metres, if any.
    [[nodiscard]] std::optional<glm::vec3> nearestPoint(const glm::vec3& position);

    /// Sends an agent (an entity with NavAgentComponent) to a point; false if it is unreachable.
    bool moveTo(entt::entity agent, const glm::vec3& target);
    void stop(entt::entity agent);
    /// True while the agent is on its way.
    [[nodiscard]] bool isMoving(entt::entity agent) const;
    /// Advances the crowd and moves the agents (through `physics` for those with a Character
    /// Controller). Builds the mesh first if needed.
    void update(float dt, PhysicsWorld* physics);

    [[nodiscard]] static bool sceneHasAgents(const Scene& scene);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lumora
