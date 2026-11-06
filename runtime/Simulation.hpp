#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lumora {

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
    /// Behaviour instances currently running (0 before the first step).
    [[nodiscard]] std::size_t scriptInstances() const noexcept;

private:
    Scene& scene_;
    double fixedStep_;
    double accumulator_ = 0.0;
    std::uint64_t steps_ = 0;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<ScriptRunner> scripts_;
};

} // namespace lumora
