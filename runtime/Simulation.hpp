#pragma once

#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <memory>

namespace lumora {

class AnimationSystem;
class AudioWorld;
class InputState;
class Scene;
class PhysicsWorld;
class ScriptModule;
class ScriptRunner;

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

    /// Runs the game library's behaviours (ScriptComponent) every step; `input` is read by
    /// keyDown() and may be null. Called again with a new library (hot reload), the running
    /// behaviours are destroyed and recreated from it.
    void setScripts(std::shared_ptr<ScriptModule> module, const InputState* input);
    /// Project folder for assets scripts load at run time (prefabs).
    void setProjectRoot(std::filesystem::path root);
    /// Plays the scene's Audio Sources (positions follow the entities every step).
    void setAudio(std::unique_ptr<AudioWorld> audio);
    [[nodiscard]] AudioWorld* audio() noexcept { return audio_.get(); }
    /// glTF animations (created when the scene has Animator components and a project root is set).
    [[nodiscard]] AnimationSystem* animation() noexcept { return animation_.get(); }
    /// Behaviour instances currently running (0 before the first step).
    [[nodiscard]] std::size_t scriptInstances() const noexcept;

private:
    Scene& scene_;
    double fixedStep_;
    double accumulator_ = 0.0;
    std::uint64_t steps_ = 0;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<AudioWorld> audio_;
    std::unique_ptr<AnimationSystem> animation_;
    std::filesystem::path projectRoot_;
    std::unique_ptr<ScriptRunner> scripts_;  // last: behaviours may still reference physics and audio
};

} // namespace lumora
