#include <cmath>
#include "runtime/Simulation.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

using namespace lumora;

TEST(Simulation, FixedStepAccumulation) {
    Scene scene;
    Simulation sim(scene, 0.01);
    EXPECT_EQ(sim.advance(0.025), 2);
    EXPECT_NEAR(sim.alpha(), 0.5, 1e-9);
    EXPECT_EQ(sim.advance(0.005), 1);
    EXPECT_EQ(sim.stepCount(), 3u);
}

TEST(Simulation, ClampsLongFramesToAvoidSpiralOfDeath) {
    Scene scene;
    Simulation sim(scene, 0.01);
    EXPECT_EQ(sim.advance(10.0), Simulation::kMaxStepsPerFrame);
}

TEST(Simulation, RotatorSpinsDeterministically) {
    auto run = [](int steps) {
        Scene scene;
        const auto e = scene.createEntity("Spinner");
        auto& rot = scene.registry().emplace<RotatorComponent>(e);
        rot.axis = {0, 1, 0};
        rot.speedDeg = 90.0f;
        Simulation sim(scene, 1.0 / 60.0);
        for (int i = 0; i < steps; ++i) sim.step();
        return scene.registry().get<TransformComponent>(e).rotationDeg;
    };
    const glm::vec3 half = run(30);  // 0.5 s at 90 deg/s
    EXPECT_NEAR(half.y, 45.0f, 1e-3f);
    EXPECT_NEAR(half.x, 0.0f, 1e-3f);
    EXPECT_EQ(run(30), run(30));
}

TEST(Simulation, OscillatorSwingsAroundItsStart) {
    Scene scene;
    const auto e = scene.createEntity("Platform");
    scene.registry().get<TransformComponent>(e).position = {1.0f, 2.0f, 0.0f};
    auto& osc = scene.registry().emplace<OscillatorComponent>(e);
    osc.offset = {0.0f, 3.0f, 0.0f};
    osc.period = 2.0f;
    Simulation sim(scene, 0.01);
    auto at = [&](int steps) {
        for (int i = 0; i < steps; ++i) sim.step();
        return scene.registry().get<TransformComponent>(e).position;
    };
    EXPECT_NEAR(at(50).y, 5.0f, 1e-4f) << "a quarter period: start + offset";
    EXPECT_NEAR(at(50).y, 2.0f, 1e-4f) << "half: back at the start";
    EXPECT_NEAR(at(50).y, -1.0f, 1e-4f) << "three quarters: start - offset";
    EXPECT_NEAR(at(50).y, 2.0f, 1e-4f) << "a full period";
    EXPECT_FLOAT_EQ(scene.registry().get<TransformComponent>(e).position.x, 1.0f);

    osc.phase = 0.25f;  // phase shifts where in the cycle it is
    EXPECT_NEAR(at(50).y, 2.0f + 3.0f * std::sin(6.2831853f * (2.5f / 2.0f + 0.25f)), 1e-4f) << "t = 2.5 s";
}
