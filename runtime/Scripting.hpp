#pragma once

#include "core/Error.hpp"
#include "runtime/GameUi.hpp"
#include "runtime/InputActions.hpp"
#include "runtime/SaveData.hpp"

#include <lumora/Script.hpp>

#include <entt/entity/entity.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lumora {

class AnimationSystem;
class AudioWorld;
class InputState;
struct HudCanvas;
struct DebugDraw;
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
    void setAudio(AudioWorld* audio) noexcept { context_.audio = audio; }
    void setAnimation(AnimationSystem* animation) noexcept { context_.animation = animation; }
    void setHud(HudCanvas* hud) noexcept { context_.hud = hud; }
    void setDebugDraw(DebugDraw* debug) noexcept { context_.debug = debug; }
    /// Project folder that spawn("Content/Prefabs/X.lprefab") resolves against.
    /// Also reads the project's input settings (Config/Input.json).
    void setProjectRoot(std::filesystem::path root);
    void setSaveData(SaveData* save) noexcept { context_.save = save; }
    void setNavigation(class NavigationWorld* navigation) noexcept { context_.navigation = navigation; }
    void setLocalization(class Localization* localization) noexcept { context_.localization = localization; }
    void setTimelines(class TimelineSystem* timelines) noexcept { context_.timelines = timelines; }
    /// 1 if a behaviour asked for the game menu since the last call, 2 for its settings page, else 0.
    [[nodiscard]] int takeMenuRequest() noexcept { return std::exchange(context_.menuRequested, 0); }
    void setMenuOpen(bool open) noexcept { context_.menuOpen = open; }
    [[nodiscard]] std::size_t instanceCount() const noexcept { return instances_.size(); }
    /// The scene a behaviour asked for with loadScene() (project-relative), once; empty if none.
    [[nodiscard]] std::string takeSceneRequest() { return std::exchange(context_.pendingScene, {}); }
    /// Destroys every behaviour instance; new ones are created on the next update (after the
    /// scene was replaced).
    void restart();

    // EngineApi::context: what the C callbacks reach (public for the callback functions).
    struct Context {
        Scene* scene = nullptr;
        const InputState* input = nullptr;
        PhysicsWorld* physics = nullptr;
        AudioWorld* audio = nullptr;
        AnimationSystem* animation = nullptr;
        HudCanvas* hud = nullptr;
        DebugDraw* debug = nullptr;
        std::filesystem::path projectRoot;
        std::unordered_map<std::string, std::shared_ptr<const nlohmann::json>> prefabs;  // loaded once
        std::vector<entt::entity> pendingDestroy;
        std::string pendingScene;  // loadScene() request, handled by the Simulation
        GameUi ui;
        InputActions actions = InputActions::defaults();
        SaveData* save = nullptr;
        class NavigationWorld* navigation = nullptr;
        class Localization* localization = nullptr;
        class TimelineSystem* timelines = nullptr;
        int menuRequested = 0;  // 1 openGameMenu(), 2 openGameSettings(); handled by the host
        bool menuOpen = false;       // set by the host
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
