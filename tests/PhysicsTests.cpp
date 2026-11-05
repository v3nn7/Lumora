#include "runtime/PhysicsWorld.hpp"
#include "runtime/Simulation.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <gtest/gtest.h>

using namespace lumora;

namespace {

entt::entity addGround(Scene& scene) {
    const entt::entity ground = scene.createEntity("Ground");
    auto& c = scene.registry().emplace<ColliderComponent>(ground);  // static: no rigid body
    c.size = {50.0f, 1.0f, 50.0f};
    scene.registry().get<TransformComponent>(ground).position.y = -0.5f;  // top surface at y = 0
    return ground;
}

entt::entity addBall(Scene& scene, glm::vec3 position, float radius = 0.5f) {
    const entt::entity ball = scene.createEntity("Ball");
    auto& c = scene.registry().emplace<ColliderComponent>(ball);
    c.shape = ColliderShape::Sphere;
    c.radius = radius;
    scene.registry().emplace<RigidBodyComponent>(ball).restitution = 0.0f;
    scene.registry().get<TransformComponent>(ball).position = position;
    return ball;
}

} // namespace

TEST(Physics, NoCollidersMeansNoWorld) {
    Scene scene;
    (void)scene.createEntity("Empty");
    Simulation sim(scene);
    EXPECT_EQ(sim.physics(), nullptr);
}

TEST(Physics, BallFallsAndComesToRestOnGround) {
    Scene scene;
    addGround(scene);
    const entt::entity ball = addBall(scene, {0.0f, 5.0f, 0.0f});
    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    EXPECT_EQ(sim.physics()->bodyCount(), 2u);

    for (int i = 0; i < 30; ++i) sim.step();  // 0.5 s: free fall
    const float y = scene.registry().get<TransformComponent>(ball).position.y;
    EXPECT_LT(y, 5.0f - 0.5f * 9.81f * 0.25f * 0.8f) << "falls under gravity";
    for (int i = 0; i < 240; ++i) sim.step();  // 4 more seconds
    const float rest = scene.registry().get<TransformComponent>(ball).position.y;
    EXPECT_NEAR(rest, 0.5f, 0.03f) << "sphere of radius 0.5 rests on the ground";
    EXPECT_LT(glm::length(sim.physics()->linearVelocity(ball)), 0.05f);
}

TEST(Physics, SimulationIsDeterministic) {
    auto run = [] {
        Scene scene;
        addGround(scene);
        std::vector<entt::entity> balls;
        for (int i = 0; i < 12; ++i) {
            balls.push_back(addBall(scene, {0.1f * static_cast<float>(i % 3), 1.0f + 1.1f * static_cast<float>(i), 0.05f * static_cast<float>(i)}));
        }
        Simulation sim(scene);
        for (int i = 0; i < 300; ++i) sim.step();
        std::vector<glm::vec3> out;
        for (entt::entity b : balls) out.push_back(scene.registry().get<TransformComponent>(b).position);
        return out;
    };
    const auto a = run();
    const auto b = run();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], b[i]) << "body " << i;
}

TEST(Physics, KinematicBodyFollowsTransformAndPushesDynamics) {
    Scene scene;
    addGround(scene);
    const entt::entity pusher = scene.createEntity("Pusher");
    auto& pc = scene.registry().emplace<ColliderComponent>(pusher);
    pc.size = {1.0f, 1.0f, 1.0f};
    scene.registry().emplace<RigidBodyComponent>(pusher).motion = BodyMotion::Kinematic;
    scene.registry().get<TransformComponent>(pusher).position = {-3.0f, 0.5f, 0.0f};
    const entt::entity ball = addBall(scene, {0.0f, 0.5f, 0.0f});
    Simulation sim(scene);
    for (int i = 0; i < 120; ++i) {
        scene.registry().get<TransformComponent>(pusher).position.x += 3.0f / 60.0f;  // 3 m/s towards +X
        sim.step();
    }
    EXPECT_NEAR(scene.registry().get<TransformComponent>(pusher).position.x, 3.0f, 1e-3f) << "kinematic is not moved by physics";
    EXPECT_GT(scene.registry().get<TransformComponent>(ball).position.x, 1.0f) << "ball was pushed";
}

TEST(Physics, DynamicChildIsWrittenBackInParentSpace) {
    Scene scene;
    addGround(scene);
    const entt::entity parent = scene.createEntity("Offset Parent");
    scene.registry().get<TransformComponent>(parent).position = {10.0f, 0.0f, 0.0f};
    const entt::entity ball = addBall(scene, {0.0f, 3.0f, 0.0f});
    scene.setParent(ball, parent);
    Simulation sim(scene);
    for (int i = 0; i < 300; ++i) sim.step();
    const auto& t = scene.registry().get<TransformComponent>(ball);
    EXPECT_NEAR(t.position.x, 0.0f, 0.02f) << "local position stays relative to the parent";
    EXPECT_NEAR(scene.worldMatrix(ball)[3].x, 10.0f, 0.02f);
    EXPECT_NEAR(scene.worldMatrix(ball)[3].y, 0.5f, 0.03f);
}

TEST(Physics, BodiesFollowEntitiesAddedOrRemovedDuringPlay) {
    Scene scene;
    addGround(scene);
    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    const entt::entity ball = addBall(scene, {0.0f, 2.0f, 0.0f});
    sim.step();
    EXPECT_EQ(sim.physics()->bodyCount(), 2u);
    scene.destroyEntity(ball);
    sim.step();
    EXPECT_EQ(sim.physics()->bodyCount(), 1u);
}

TEST(Physics, ComponentsSerialize) {
    Scene scene;
    const entt::entity ball = addBall(scene, {1, 2, 3}, 0.75f);
    scene.registry().get<RigidBodyComponent>(ball).mass = 7.5f;
    auto copy = SceneSerializer::clone(scene);
    ASSERT_TRUE(copy.has_value());
    const entt::entity e = (*copy)->findByUuid(scene.uuidOf(ball));
    EXPECT_FLOAT_EQ((*copy)->registry().get<RigidBodyComponent>(e).mass, 7.5f);
    EXPECT_EQ((*copy)->registry().get<ColliderComponent>(e).shape, ColliderShape::Sphere);
    EXPECT_FLOAT_EQ((*copy)->registry().get<ColliderComponent>(e).radius, 0.75f);
}
