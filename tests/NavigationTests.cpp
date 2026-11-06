#include "runtime/Navigation.hpp"
#include "runtime/PhysicsWorld.hpp"
#include "runtime/Simulation.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

using namespace lumora;

namespace {

entt::entity box(Scene& scene, const char* name, glm::vec3 center, glm::vec3 size) {
    const entt::entity e = scene.createEntity(name);
    scene.registry().get<TransformComponent>(e).position = center;
    scene.registry().emplace<ColliderComponent>(e).size = size;
    return e;
}

/// A 20 x 20 m floor (top at y = 0) split by a wall along z = 0 with a gap at x 6..10, and a
/// separate platform far away.
void buildLevel(Scene& scene) {
    box(scene, "Floor", {0.0f, -0.5f, 0.0f}, {20.0f, 1.0f, 20.0f});
    box(scene, "Wall", {-2.0f, 1.0f, 0.0f}, {16.0f, 2.0f, 0.4f});  // x -10..6
    box(scene, "Island", {40.0f, -0.5f, 0.0f}, {4.0f, 1.0f, 4.0f});
    scene.updateWorldTransforms();
}

float length(const std::vector<glm::vec3>& path) {
    float sum = 0.0f;
    for (std::size_t i = 1; i < path.size(); ++i) sum += glm::distance(path[i - 1], path[i]);
    return sum;
}

} // namespace

TEST(Navigation, PathsGoAroundWallsThroughTheGap) {
    Scene scene;
    buildLevel(scene);
    NavigationWorld nav(scene);
    ASSERT_TRUE(nav.build().has_value());
    EXPECT_GT(nav.polygonCount(), 2u);
    const auto path = nav.findPath({0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 5.0f});
    ASSERT_GE(path.size(), 3u) << "has to turn";
    EXPECT_NEAR(path.front().z, -5.0f, 0.3f);
    EXPECT_NEAR(path.back().z, 5.0f, 0.3f);
    bool throughGap = false;
    for (const glm::vec3& p : path) throughGap = throughGap || (p.x > 5.8f && std::abs(p.z) < 1.0f);
    EXPECT_TRUE(throughGap) << "the only way is the gap at x 6..10";
    EXPECT_GT(length(path), 16.0f) << "much longer than the straight 10 m";
    EXPECT_TRUE(nav.findPath({0.0f, 0.0f, -5.0f}, {40.0f, 0.0f, 0.0f}).empty()) << "the island cannot be reached";
}

TEST(Navigation, AgentsWalkToTheirTarget) {
    Scene scene;
    buildLevel(scene);
    const entt::entity walker = scene.createEntity("Walker");
    scene.registry().get<TransformComponent>(walker).position = {0.0f, 0.0f, -5.0f};
    scene.registry().emplace<NavAgentComponent>(walker).speed = 4.0f;
    // A second agent walks through the physics as a character.
    const entt::entity runner = scene.createEntity("Runner");
    scene.registry().get<TransformComponent>(runner).position = {-4.0f, 0.0f, -6.0f};
    scene.registry().emplace<NavAgentComponent>(runner).speed = 4.0f;
    scene.registry().emplace<CharacterControllerComponent>(runner);
    scene.updateWorldTransforms();

    Simulation sim(scene);
    NavigationWorld& nav = sim.navigation();
    sim.step();  // agents join the crowd
    ASSERT_TRUE(nav.moveTo(walker, {0.0f, 0.0f, 5.0f}));
    ASSERT_TRUE(nav.moveTo(runner, {-4.0f, 0.0f, 6.0f}));
    EXPECT_TRUE(nav.isMoving(walker));
    EXPECT_FALSE(nav.moveTo(walker, {40.0f, 0.0f, 0.0f})) << "unreachable target refused";
    for (int i = 0; i < 60 * 15 && (nav.isMoving(walker) || nav.isMoving(runner)); ++i) sim.step();
    EXPECT_FALSE(nav.isMoving(walker)) << "arrived";
    const glm::vec3 w = scene.registry().get<TransformComponent>(walker).position;
    EXPECT_NEAR(w.x, 0.0f, 0.5f);
    EXPECT_NEAR(w.z, 5.0f, 0.5f);
    const glm::vec3 r = scene.registry().get<TransformComponent>(runner).position;
    EXPECT_NEAR(r.x, -4.0f, 0.6f) << "the character got there through the physics";
    EXPECT_NEAR(r.z, 6.0f, 0.6f);
    EXPECT_NEAR(r.y, 0.0f, 0.1f) << "standing on the floor";
}
