#pragma once

#include <cstddef>
#include <filesystem>

#include "core/Error.hpp"
#include "scene/DebugDraw.hpp"
#include "scene/Hud.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

namespace lumora {

class AnimationSystem;
class AudioWorld;
class InputState;
class Scene;
class PhysicsWorld;
class ScriptModule;
class ScriptRunner;
class SaveData;
class NavigationWorld;
class Localization;
class TimelineSystem;

/// Drives gameplay systems at a fixed timestep, decoupled from the render frame rate.
/// Given the same sequence of step counts the result is deterministic.
class Simulation {
public:
    static constexpr double kDefaultFixedStep = 1.0 / 60.0;
    static constexpr int kMaxStepsPerFrame = 8;  // avoids the spiral of death after hitches

    explicit Simulation(Scene& scene, double fixedStep = kDefaultFixedStep);
    ~Simulation();
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    /// Accumulates real time and runs as many fixed steps as fit. Returns steps executed.
    int advance(double frameSeconds);
    /// Runs exactly one fixed step (used by "step frame" and tests).
    void step();

    [[nodiscard]] double fixedStep() const noexcept { return fixedStep_; }
    [[nodiscard]] std::uint64_t stepCount() const noexcept { return steps_; }
    [[nodiscard]] double simulatedSeconds() const noexcept { return static_cast<double>(steps_) * fixedStep_; }
    /// Fraction of a step left in the accumulator, for render interpolation.
    [[nodiscard]] double alpha() const noexcept { return accumulator_ / fixedStep_; }
    /// Physics world, present when the scene contains colliders at start.
    [[nodiscard]] PhysicsWorld* physics() noexcept { return physics_.get(); }
    /// Navigation mesh and agents (the mesh is built from the static colliders on first use).
    [[nodiscard]] NavigationWorld& navigation() noexcept { return *navigation_; }

    /// Runs the game library's behaviours (ScriptComponent) every step; `input` is read by
    /// keyDown() and may be null. Called again with a new library (hot reload), the running
    /// behaviours are destroyed and recreated from it.
    void setScripts(std::shared_ptr<ScriptModule> module, const InputState* input);
    /// Project folder for assets scripts load at run time (prefabs).
    void setProjectRoot(std::filesystem::path root);
    /// Replaces the scene's contents with a scene file (project-relative) and starts it over:
    /// behaviours, physics, animation and sound are recreated and time() restarts at 0. Scripts
    /// ask for it with loadScene(); the switch happens at the end of their step.
    Result<> loadScene(const std::filesystem::path& relativePath);
    /// Counts scene switches, so hosts can notice one (and refresh what shows the scene).
    [[nodiscard]] std::uint64_t sceneLoads() const noexcept { return sceneLoads_; }
    /// The scene file last loaded with loadScene() (project-relative), empty before any.
    [[nodiscard]] const std::filesystem::path& loadedScene() const noexcept { return loadedScene_; }
    /// Where the game's saved values live (behaviours' saveNumber/loadText); read now, written
    /// while playing and when the simulation ends.
    void setSaveFile(std::filesystem::path file);
    [[nodiscard]] SaveData* saveData() noexcept { return save_.get(); }
    /// Plays the scene's Audio Sources (positions follow the entities every step).
    void setAudio(std::unique_ptr<AudioWorld> audio);
    [[nodiscard]] AudioWorld* audio() noexcept { return audio_.get(); }
    /// glTF animations (created when the scene has Animator components and a project root is set).
    [[nodiscard]] AnimationSystem* animation() noexcept { return animation_.get(); }
    /// The game's HUD as the behaviours drew it in the latest step (cleared at every step).
    [[nodiscard]] HudCanvas& hud() noexcept { return hud_; }
    /// Debug shapes the behaviours drew in the latest step (cleared at every step).
    [[nodiscard]] DebugDraw& debugDraw() noexcept { return debug_; }
    /// Timeline components (cutscenes); recreated with the project root and on scene loads.
    [[nodiscard]] TimelineSystem& timelines() noexcept { return *timelines_; }
    /// The game's texts (Content/Localization), read with the project root.
    [[nodiscard]] Localization& localization() noexcept { return *localization_; }
    /// 1 if a behaviour asked for the built-in game menu since the last call (the host shows
    /// it), 2 for its settings page, 0 if not.
    [[nodiscard]] int takeGameMenuRequest();
    /// Tells behaviours whether the host's game menu is open (gameMenuOpen()).
    void setGameMenuOpen(bool open);
    /// Behaviour instances currently running (0 before the first step).
    [[nodiscard]] std::size_t scriptInstances() const noexcept;

private:
    void runOscillators(float dt);
    std::unordered_map<entt::entity, glm::vec3> oscillatorOrigins_;
    Scene& scene_;
    double fixedStep_;
    double accumulator_ = 0.0;
    std::uint64_t steps_ = 0;
    std::uint64_t sceneLoads_ = 0;
    std::filesystem::path loadedScene_;
    bool audioDevice_ = true;  // whether sound for a loaded scene goes to the audio device
    std::unique_ptr<SaveData> save_;
    std::unique_ptr<NavigationWorld> navigation_;
    std::unique_ptr<Localization> localization_;
    std::unique_ptr<TimelineSystem> timelines_;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<AudioWorld> audio_;
    std::unique_ptr<AnimationSystem> animation_;
    std::filesystem::path projectRoot_;
    HudCanvas hud_;
    DebugDraw debug_;
    std::unique_ptr<ScriptRunner> scripts_;  // last: behaviours may still reference physics and audio
};

} // namespace lumora
