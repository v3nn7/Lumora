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

TEST(Character, WalksClimbsStepsStopsAtWallsAndJumps) {
    Scene scene("Character");
    auto& reg = scene.registry();
    auto box = [&](const char* name, glm::vec3 center, glm::vec3 size) {
        const entt::entity e = scene.createEntity(name);
        reg.get<TransformComponent>(e).position = center;
        reg.emplace<ColliderComponent>(e).size = size;
        return e;
    };
    box("Ground", {0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f});      // top at y = 0
    box("Step", {3.0f, 0.1f, 0.0f}, {2.0f, 0.2f, 4.0f});           // 20 cm high, x 2..4
    box("Wall", {8.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 4.0f});           // x 7.5..8.5
    const entt::entity player = scene.createEntity("Player");
    reg.get<TransformComponent>(player).position = glm::vec3(0.0f, 1.0f, 0.0f);  // dropped from 1 m
    reg.emplace<CharacterControllerComponent>(player);
    scene.updateWorldTransforms();

    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    PhysicsWorld& physics = *sim.physics();
    ASSERT_EQ(physics.characterCount(), 1u);
    for (int i = 0; i < 60; ++i) sim.step();
    EXPECT_NEAR(reg.get<TransformComponent>(player).position.y, 0.0f, 0.05f) << "fell and stands on the ground";
    EXPECT_TRUE(physics.characterGrounded(player));

    // Walk +x at 2 m/s for 2.5 s: over the step (climbed, not blocked).
    physics.moveCharacter(player, {2.0f, 0.0f, 0.0f});
    for (int i = 0; i < 90; ++i) sim.step();
    const glm::vec3 onStep = reg.get<TransformComponent>(player).position;
    EXPECT_GT(onStep.x, 2.3f);
    EXPECT_LT(onStep.x, 4.0f);
    EXPECT_NEAR(onStep.y, 0.2f, 0.06f) << "stands on top of the step";

    // Keep walking into the wall: stops in front of it.
    for (int i = 0; i < 240; ++i) sim.step();
    const glm::vec3 atWall = reg.get<TransformComponent>(player).position;
    EXPECT_LT(atWall.x, 7.5f - 0.25f) << "the capsule (radius 0.3) stays out of the wall";
    EXPECT_GT(atWall.x, 6.5f);

    // Jump: rises, lands again.
    physics.moveCharacter(player, {0.0f, 0.0f, 0.0f});
    EXPECT_TRUE(physics.jumpCharacter(player, 5.0f));
    float peak = 0.0f;
    for (int i = 0; i < 90; ++i) {
        sim.step();
        peak = std::max(peak, reg.get<TransformComponent>(player).position.y);
    }
    EXPECT_GT(peak, 1.0f) << "v^2 / 2g = 1.27 m";
    EXPECT_LT(peak, 1.5f);
    EXPECT_NEAR(reg.get<TransformComponent>(player).position.y, 0.0f, 0.05f) << "landed";
    EXPECT_TRUE(physics.characterGrounded(player));
}

namespace {

bool touched(const std::vector<std::pair<entt::entity, entt::entity>>& pairs, entt::entity a, entt::entity b) {
    for (const auto& [x, y] : pairs) {
        if ((x == a && y == b) || (x == b && y == a)) return true;
    }
    return false;
}

} // namespace

TEST(Physics, TriggerReportsContactsButBlocksNothing) {
    Scene scene;
    addGround(scene);
    const entt::entity trigger = scene.createEntity("Trigger");
    auto& c = scene.registry().emplace<ColliderComponent>(trigger);
    c.size = {4.0f, 1.0f, 4.0f};
    c.isTrigger = true;
    scene.registry().get<TransformComponent>(trigger).position.y = 3.0f;
    const entt::entity ball = addBall(scene, {0.0f, 6.0f, 0.0f});
    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    std::vector<std::pair<entt::entity, entt::entity>> contacts;
    for (int i = 0; i < 240; ++i) {
        sim.step();
        auto step = sim.physics()->takeContacts();
        contacts.insert(contacts.end(), step.begin(), step.end());
    }
    EXPECT_TRUE(touched(contacts, ball, trigger)) << "falling through the trigger is reported";
    EXPECT_NEAR(scene.registry().get<TransformComponent>(ball).position.y, 0.5f, 0.03f)
        << "and the ball fell through it onto the ground";
}

TEST(Character, TouchingBodiesAndTriggersIsReported) {
    Scene scene("Character");
    auto& reg = scene.registry();
    const entt::entity ground = addGround(scene);
    const entt::entity trigger = scene.createEntity("Coin");
    auto& c = reg.emplace<ColliderComponent>(trigger);
    c.size = {1.0f, 2.0f, 1.0f};
    c.isTrigger = true;
    reg.get<TransformComponent>(trigger).position = {3.0f, 1.0f, 0.0f};
    const entt::entity player = scene.createEntity("Player");
    reg.emplace<CharacterControllerComponent>(player);
    scene.updateWorldTransforms();

    Simulation sim(scene);
    PhysicsWorld& physics = *sim.physics();
    std::vector<std::pair<entt::entity, entt::entity>> contacts;
    auto run = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            sim.step();
            auto step = physics.takeContacts();
            contacts.insert(contacts.end(), step.begin(), step.end());
        }
    };
    run(30);
    EXPECT_TRUE(touched(contacts, player, ground)) << "landing on the ground";
    EXPECT_FALSE(touched(contacts, player, trigger));
    physics.moveCharacter(player, {3.0f, 0.0f, 0.0f});
    run(120);  // 2 s at 3 m/s: through the coin
    EXPECT_TRUE(touched(contacts, player, trigger)) << "walking into the trigger";
    EXPECT_GT(reg.get<TransformComponent>(player).position.x, 5.0f) << "it does not block the character";
}

TEST(Character, RidesAMovingPlatform) {
    Scene scene("Platform");
    auto& reg = scene.registry();
    const entt::entity platform = scene.createEntity("Platform");
    reg.emplace<ColliderComponent>(platform).size = {4.0f, 0.5f, 4.0f};
    reg.emplace<RigidBodyComponent>(platform).motion = BodyMotion::Kinematic;
    auto& osc = reg.emplace<OscillatorComponent>(platform);
    osc.offset = {3.0f, 0.0f, 0.0f};
    osc.period = 6.0f;
    reg.get<TransformComponent>(platform).position = {0.0f, -0.25f, 0.0f};  // top at y = 0
    const entt::entity player = scene.createEntity("Player");
    reg.emplace<CharacterControllerComponent>(player);
    reg.get<TransformComponent>(player).position = {0.0f, 0.05f, 0.0f};
    scene.updateWorldTransforms();

    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    for (int i = 0; i < 10; ++i) sim.step();  // landed
    ASSERT_TRUE(sim.physics()->characterGrounded(player));
    const float startOffset = reg.get<TransformComponent>(player).position.x - reg.get<TransformComponent>(platform).position.x;
    for (int i = 0; i < 80; ++i) sim.step();  // to a quarter period: the platform moved 3 m
    EXPECT_NEAR(reg.get<TransformComponent>(platform).position.x, 3.0f, 0.01f);
    const glm::vec3 p = reg.get<TransformComponent>(player).position;
    EXPECT_NEAR(p.x - 3.0f, startOffset, 0.05f) << "carried along (it would have fallen off otherwise)";
    EXPECT_NEAR(p.y, 0.0f, 0.1f) << "still standing on top";
    EXPECT_TRUE(sim.physics()->characterGrounded(player));
}

TEST(Joints, HingeHoldsADoorThatSwingsAndAMotorTurnsIt) {
    Scene scene;
    auto& reg = scene.registry();
    const entt::entity door = scene.createEntity("Door");
    reg.emplace<ColliderComponent>(door).size = {1.0f, 2.0f, 0.1f};
    reg.emplace<RigidBodyComponent>(door);
    reg.get<TransformComponent>(door).position = {0.5f, 1.0f, 0.0f};
    auto& joint = reg.emplace<JointComponent>(door);  // hinge on the left edge, vertical axis, to the world
    joint.type = JointType::Hinge;
    joint.anchor = {-0.5f, 0.0f, 0.0f};
    joint.axis = {0.0f, 1.0f, 0.0f};
    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    for (int i = 0; i < 60; ++i) sim.step();
    EXPECT_EQ(sim.physics()->jointCount(), 1u);
    EXPECT_NEAR(reg.get<TransformComponent>(door).position.y, 1.0f, 0.02f) << "the hinge holds it up";

    joint.motorForce = 500.0f;
    joint.motorSpeed = 90.0f;  // deg/s
    for (int i = 0; i < 60; ++i) sim.step();  // 1 s
    const glm::vec3 p = reg.get<TransformComponent>(door).position;
    EXPECT_NEAR(glm::length(glm::vec2(p.x, p.z)), 0.5f, 0.03f) << "turns around the hinge";
    EXPECT_GT(std::abs(reg.get<TransformComponent>(door).rotationDeg.y), 45.0f);
}

TEST(Joints, BallJointMakesAPendulumAndConnectsBodies) {
    Scene scene;
    auto& reg = scene.registry();
    const entt::entity bob = addBall(scene, {1.0f, 3.0f, 0.0f}, 0.2f);
    auto& joint = reg.emplace<JointComponent>(bob);
    joint.type = JointType::Ball;
    joint.anchor = {-1.0f, 0.0f, 0.0f};  // pivot 1 m to the left, fixed to the world
    const entt::entity second = addBall(scene, {2.0f, 3.0f, 0.0f}, 0.2f);
    scene.rename(bob, "Bob");
    auto& link = reg.emplace<JointComponent>(second);
    link.type = JointType::Distance;
    link.connectedTo = "Bob";
    Simulation sim(scene);
    for (int i = 0; i < 120; ++i) {
        sim.step();
        const glm::vec3 p = reg.get<TransformComponent>(bob).position;
        ASSERT_NEAR(glm::length(p - glm::vec3(0.0f, 3.0f, 0.0f)), 1.0f, 0.05f) << "stays on the pendulum's circle";
    }
    EXPECT_EQ(sim.physics()->jointCount(), 2u);
    EXPECT_LT(reg.get<TransformComponent>(bob).position.y, 2.5f) << "swings down";
    const float rope = glm::length(reg.get<TransformComponent>(second).position - reg.get<TransformComponent>(bob).position);
    EXPECT_NEAR(rope, 1.0f, 0.05f) << "the distance joint keeps the second ball at its starting distance";

    reg.remove<JointComponent>(bob);
    sim.step();
    EXPECT_EQ(sim.physics()->jointCount(), 1u);
    reg.remove<ColliderComponent>(bob);  // the body goes: so does the joint on it
    sim.step();
    EXPECT_EQ(sim.physics()->jointCount(), 0u);
}

TEST(Vehicle, DrivesForwardAndSteers) {
    Scene scene;
    auto& reg = scene.registry();
    addGround(scene);
    const entt::entity car = scene.createEntity("Car");
    reg.emplace<ColliderComponent>(car).size = {1.8f, 0.6f, 4.0f};
    reg.emplace<RigidBodyComponent>(car).mass = 1200.0f;
    reg.get<TransformComponent>(car).position = {0.0f, 1.2f, 0.0f};
    reg.emplace<VehicleComponent>(car);
    const entt::entity wheel = scene.createEntity("Wheel FL", Uuid::generate(), car);
    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    for (int i = 0; i < 90; ++i) sim.step();  // settle on the suspension
    EXPECT_EQ(sim.physics()->vehicleCount(), 1u);
    const glm::vec3 start = reg.get<TransformComponent>(car).position;
    EXPECT_GT(start.y, 0.4f) << "stands on its wheels";

    for (int i = 0; i < 180; ++i) {
        ASSERT_TRUE(sim.physics()->driveVehicle(car, 1.0f, 0.0f, 0.0f, 0.0f));
        sim.step();
    }
    const glm::vec3 driven = reg.get<TransformComponent>(car).position;
    EXPECT_LT(driven.z, start.z - 5.0f) << "drives forward (-Z)";
    EXPECT_GT(sim.physics()->vehicleSpeed(car), 2.0f);
    EXPECT_NEAR(driven.x, start.x, 0.5f) << "straight";
    const glm::vec3 wheelPos = reg.get<TransformComponent>(wheel).position;
    EXPECT_LT(wheelPos.x, 0.0f) << "the front-left wheel entity follows its wheel";
    EXPECT_LT(wheelPos.z, 0.0f);

    for (int i = 0; i < 120; ++i) {
        sim.physics()->driveVehicle(car, 0.5f, 1.0f, 0.0f, 0.0f);
        sim.step();
    }
    EXPECT_GT(reg.get<TransformComponent>(car).position.x, driven.x + 1.0f) << "steering right turns right";
}

TEST(Ragdoll, BonesFallAsBodiesWhenActivated) {
    Scene scene;
    auto& reg = scene.registry();
    addGround(scene);
    const entt::entity root = scene.createEntity("Model");
    reg.get<TransformComponent>(root).position = {0.0f, 1.0f, 0.0f};
    // A three-bone chain standing upright: hips -> spine -> head.
    entt::entity parent = root;
    std::vector<entt::entity> bones;
    for (int i = 0; i < 3; ++i) {
        const entt::entity bone = scene.createEntity("Bone" + std::to_string(i), Uuid::generate(), parent);
        reg.emplace<ModelNodeComponent>(bone).node = i;
        if (i > 0) reg.get<TransformComponent>(bone).position.y = 0.4f;
        bones.push_back(bone);
        parent = bone;
    }
    auto& ragdoll = reg.emplace<RagdollComponent>(root);
    (void)addBall(scene, {5.0f, 0.5f, 5.0f});
    Simulation sim(scene);
    ASSERT_NE(sim.physics(), nullptr);
    sim.step();
    EXPECT_EQ(sim.physics()->ragdollBodyCount(), 0u) << "inactive until switched on";

    ragdoll.active = true;
    for (int i = 0; i < 240; ++i) sim.step();
    EXPECT_EQ(sim.physics()->ragdollBodyCount(), 3u);
    scene.updateWorldTransforms();
    const float headY = scene.worldMatrix(bones[2])[3].y;
    EXPECT_LT(headY, 1.4f) << "the chain collapsed";
    EXPECT_GT(headY, -0.05f) << "onto the ground, not through it";

    ragdoll.active = false;
    sim.step();
    EXPECT_EQ(sim.physics()->ragdollBodyCount(), 0u);
}

TEST(Physics, JointVehicleRagdollComponentsSerialize) {
    Scene scene;
    const entt::entity e = scene.createEntity("E");
    auto& j = scene.registry().emplace<JointComponent>(e);
    j.type = JointType::Slider;
    j.connectedTo = "Frame";
    j.motorSpeed = 2.5f;
    scene.registry().emplace<VehicleComponent>(e).engineTorque = 900.0f;
    scene.registry().emplace<RagdollComponent>(e).active = true;
    auto cloned = SceneSerializer::clone(scene);
    ASSERT_TRUE(cloned.has_value());
    Scene& copy = **cloned;
    const entt::entity c = copy.findByUuid(scene.uuidOf(e));
    EXPECT_EQ(copy.registry().get<JointComponent>(c).type, JointType::Slider);
    EXPECT_EQ(copy.registry().get<JointComponent>(c).connectedTo, "Frame");
    EXPECT_FLOAT_EQ(copy.registry().get<JointComponent>(c).motorSpeed, 2.5f);
    EXPECT_FLOAT_EQ(copy.registry().get<VehicleComponent>(c).engineTorque, 900.0f);
    EXPECT_TRUE(copy.registry().get<RagdollComponent>(c).active);
}
