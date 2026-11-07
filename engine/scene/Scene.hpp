#pragma once

#include "core/Uuid.hpp"
#include "scene/Components.hpp"

#include <entt/entity/registry.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lumora {

/// Owns an EnTT registry plus the scene graph and UUID index.
/// Root and child order is explicit so hierarchy display and serialization are stable.
class Scene {
public:
    explicit Scene(std::string name = "Untitled");
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] entt::entity createEntity(std::string_view name, Uuid id = Uuid::generate(),
                                            entt::entity parent = entt::null);
    /// Destroys the entity and all of its descendants.
    void destroyEntity(entt::entity entity);
    /// Removes every entity and resets the environment (the Scene object itself stays valid).
    void clear();
    /// Deep-copies an entity subtree (new UUIDs) and inserts it next to the original.
    entt::entity duplicateEntity(entt::entity entity);

    /// Re-parents keeping the local transform. Returns false if it would create a cycle.
    bool setParent(entt::entity child, entt::entity parent, std::size_t index = SIZE_MAX);
    [[nodiscard]] entt::entity parentOf(entt::entity entity) const;
    [[nodiscard]] const std::vector<entt::entity>& childrenOf(entt::entity entity) const;
    [[nodiscard]] const std::vector<entt::entity>& roots() const noexcept { return roots_; }
    [[nodiscard]] bool isAncestor(entt::entity ancestor, entt::entity entity) const;

    [[nodiscard]] bool isValid(entt::entity entity) const { return registry_.valid(entity); }
    [[nodiscard]] entt::entity findByUuid(Uuid id) const;
    [[nodiscard]] Uuid uuidOf(entt::entity entity) const;
    /// Gives an entity another UUID (network games: a client's copy takes the host's). False if
    /// the UUID is invalid or another entity has it.
    bool changeUuid(entt::entity entity, Uuid id);
    [[nodiscard]] const std::string& nameOf(entt::entity entity) const;
    void rename(entt::entity entity, std::string name);
    [[nodiscard]] std::size_t entityCount() const noexcept { return uuidMap_.size(); }

    void updateWorldTransforms();
    [[nodiscard]] glm::mat4 worldMatrix(entt::entity entity) const;

    /// First entity with a primary CameraComponent (or any camera), entt::null otherwise.
    [[nodiscard]] entt::entity primaryCamera() const;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] EnvironmentSettings& environment() noexcept { return environment_; }
    [[nodiscard]] const EnvironmentSettings& environment() const noexcept { return environment_; }

    /// Depth-first traversal in hierarchy order.
    template <class Fn>
    void forEachDepthFirst(Fn&& fn) const {
        for (entt::entity root : roots_) {
            visit(root, 0, fn);
        }
    }

private:
    template <class Fn>
    void visit(entt::entity e, int depth, Fn& fn) const {
        fn(e, depth);
        for (entt::entity child : childrenOf(e)) {
            visit(child, depth + 1, fn);
        }
    }

    void detach(entt::entity entity);
    void updateWorldRecursive(entt::entity entity, const glm::mat4& parentWorld);
    entt::entity cloneRecursive(entt::entity source, entt::entity newParent);

    std::string name_;
    entt::registry registry_;
    std::unordered_map<Uuid, entt::entity> uuidMap_;
    std::vector<entt::entity> roots_;
    EnvironmentSettings environment_;
};

} // namespace lumora
