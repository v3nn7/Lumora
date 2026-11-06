#pragma once

#include "core/Error.hpp"

#include <lumora/Script.hpp>

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lumora {

class InputState;
class PhysicsWorld;
class Scene;
struct ScriptComponent;

/// A loaded game library (the project's Scripts/ compiled into a shared library). The file is
/// first copied to a unique name in `loadDir`, so a rebuild can overwrite the original while
/// this copy is in use, and the OS never hands back an older library cached under the same
/// path. Dropping the last reference unloads it and deletes the copy.
class ScriptModule {
public:
    [[nodiscard]] static Result<std::shared_ptr<ScriptModule>> load(const std::filesystem::path& library,
                                                                    const std::filesystem::path& loadDir);
    ~ScriptModule();
    ScriptModule(const ScriptModule&) = delete;
    ScriptModule& operator=(const ScriptModule&) = delete;

    /// Behaviour names in registration order.
    [[nodiscard]] const std::vector<std::string>& behaviours() const noexcept { return names_; }
    /// Index of a behaviour, or -1.
    [[nodiscard]] int find(std::string_view name) const noexcept;

    // The library's C entry points (see lumora/ScriptModuleMain.hpp).
    using CreateFn = void* (*)(std::uint32_t, const script::EngineApi*, script::Entity);
    using StepFn = int (*)(void*);
    using UpdateFn = int (*)(void*, float);
    using DestroyFn = void (*)(void*);
    using CollisionFn = int (*)(void*, script::Entity);
    using ErrorFn = const char* (*)();
    CreateFn create = nullptr;
    StepFn start = nullptr;
    UpdateFn update = nullptr;
    DestroyFn destroy = nullptr;
    CollisionFn collision = nullptr;
    ErrorFn lastError = nullptr;

private:
    ScriptModule() = default;
    void* handle_ = nullptr;
    std::filesystem::path copy_;
    std::vector<std::string> names_;
};

/// The behaviour instances of one simulation: one per entity with a ScriptComponent whose
/// behaviour the module provides. Instances are created (and start() runs) on the first
/// update; reload() destroys them, swaps the module and starts fresh instances.
class ScriptRunner {
public:
    ScriptRunner(Scene& scene, std::shared_ptr<ScriptModule> module, const InputState* input);
    ~ScriptRunner();
    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    /// Runs every behaviour's update(); then removes entities scripts destroyed and starts
    /// behaviours on entities that appeared (clones).
    void update(float dt, double simulatedSeconds);
    /// Calls onCollision on both entities of each pair that has a behaviour.
    void dispatchCollisions(const std::vector<std::pair<entt::entity, entt::entity>>& pairs);
    void reload(std::shared_ptr<ScriptModule> module);
    void setPhysics(PhysicsWorld* physics) noexcept { context_.physics = physics; }
    [[nodiscard]] std::size_t instanceCount() const noexcept { return instances_.size(); }

    // EngineApi::context: what the C callbacks reach (public for the callback functions).
    struct Context {
        Scene* scene = nullptr;
        const InputState* input = nullptr;
        PhysicsWorld* physics = nullptr;
        std::vector<entt::entity> pendingDestroy;
        double time = 0.0;
        std::string nameScratch;
    };

private:
    struct Instance {
        std::uint64_t entity;
        std::string behaviour;
        void* object;
    };
    void createInstances();
    void startNewInstances();  // behaviours on entities without one yet (e.g. clones)
    bool createInstance(entt::entity entity, const ScriptComponent& script);
    void finishStep();
    void destroyInstances();
    void reportFailure(const Instance& instance, const char* phase);

    Scene& scene_;
    std::shared_ptr<ScriptModule> module_;
    Context context_;
    script::EngineApi api_{};
    std::vector<Instance> instances_;
    std::unordered_set<std::uint64_t> scripted_;  // entities that got an instance (or were skipped)
    bool created_ = false;
};

} // namespace lumora
