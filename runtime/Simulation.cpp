#include "runtime/Simulation.hpp"

#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cmath>

namespace lumora {

namespace {

/// Rotates entities with a RotatorComponent around their (local) axis.
void runRotatorSystem(Scene& scene, float dt) {
    auto view = scene.registry().view<RotatorComponent, TransformComponent>();
    for (auto [e, rotator, transform] : view.each()) {
        const float len = glm::length(rotator.axis);
        if (len < 1e-6f || rotator.speedDeg == 0.0f) continue;
        const glm::vec3 axis = rotator.axis / len;
        const glm::vec3 r = glm::radians(transform.rotationDeg);
        const glm::mat4 current = glm::eulerAngleYXZ(r.y, r.x, r.z);
        const glm::mat4 delta = glm::mat4_cast(glm::angleAxis(glm::radians(rotator.speedDeg * dt), axis));
        float y = 0.0f, x = 0.0f, z = 0.0f;
        glm::extractEulerAngleYXZ(current * delta, y, x, z);
        transform.rotationDeg = glm::degrees(glm::vec3(x, y, z));
    }
}

} // namespace

Simulation::Simulation(Scene& scene, double fixedStep) : scene_(scene), fixedStep_(fixedStep > 0.0 ? fixedStep : kDefaultFixedStep) {}

int Simulation::advance(double frameSeconds) {
    accumulator_ += std::clamp(frameSeconds, 0.0, fixedStep_ * kMaxStepsPerFrame);
    int executed = 0;
    // Small tolerance so accumulated rounding error never drops a whole step.
    constexpr double kTolerance = 1e-9;
    while (accumulator_ + kTolerance >= fixedStep_ && executed < kMaxStepsPerFrame) {
        step();
        accumulator_ = std::max(0.0, accumulator_ - fixedStep_);
        ++executed;
    }
    return executed;
}

void Simulation::step() {
    const auto dt = static_cast<float>(fixedStep_);
    runRotatorSystem(scene_, dt);
    scene_.updateWorldTransforms();
    ++steps_;
}

} // namespace lumora
