#pragma once

#include <cstdint>
#include <memory>

namespace lumora {

class Scene;
class PhysicsWorld;

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

private:
    Scene& scene_;
    double fixedStep_;
    double accumulator_ = 0.0;
    std::uint64_t steps_ = 0;
    std::unique_ptr<PhysicsWorld> physics_;
};

} // namespace lumora
