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
