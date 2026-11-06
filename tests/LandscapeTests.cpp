#include "TestUtils.hpp"

#include "runtime/Navigation.hpp"
#include "runtime/PhysicsWorld.hpp"
#include "runtime/Simulation.hpp"
#include "scene/Landscape.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <gtest/gtest.h>
#include <stb_image_write.h>

#include <vector>

using namespace lumora;

TEST(Terrain, GeneratedHillsAreDeterministicAndFollowTheSettings) {
    TerrainComponent t;
    t.size = 100.0f;
    t.height = 20.0f;
    t.resolution = 65;
    const auto a = terrainData(t, {});
    ASSERT_EQ(a->heights.size(), 65u * 65u);
    float lo = 1e9f, hi = -1e9f;
    for (const float h : a->heights) {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    EXPECT_GE(lo, 0.0f);
    EXPECT_LE(hi, 20.0f);
    EXPECT_GT(hi - lo, 3.0f) << "hills, not a plane";
    TerrainComponent same = t;
    EXPECT_EQ(terrainData(same, {})->heights, a->heights) << "same settings, same ground";
    t.seed = 7;
    EXPECT_NE(terrainData(t, {})->heights, a->heights) << "another seed, other hills";
    // Mesh: a vertex per sample, two triangles per cell, facing up.
    const MeshData mesh = buildTerrainMesh(*a, 4.0f);
    EXPECT_EQ(mesh.vertices.size(), 65u * 65u);
    EXPECT_EQ(mesh.indices.size(), 64u * 64u * 6u);
    const glm::vec3 p0 = mesh.vertices[mesh.indices[0]].position, p1 = mesh.vertices[mesh.indices[1]].position,
                    p2 = mesh.vertices[mesh.indices[2]].position;
    EXPECT_GT(glm::cross(p1 - p0, p2 - p0).y, 0.0f) << "counter-clockwise from above";
    EXPECT_NEAR(a->heightAt(-50.0f, -50.0f), a->heights.front(), 1e-4f) << "the corner sample";
}

TEST(Terrain, ReadsAHeightmap) {
    test::TempDir dir;
    // A ramp: black at the left (-X), white at the right (+X).
    std::vector<std::uint8_t> pixels(32 * 8 * 3);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 32; ++x) {
            const auto v = static_cast<std::uint8_t>(x * 255 / 31);
            for (int c = 0; c < 3; ++c) pixels[static_cast<std::size_t>((y * 32 + x) * 3 + c)] = v;
        }
    }
    ASSERT_NE(stbi_write_png((dir.path() / "ramp.png").string().c_str(), 32, 8, 3, pixels.data(), 32 * 3), 0);
    TerrainComponent t;
    t.size = 10.0f;
    t.height = 4.0f;
    t.resolution = 33;
    t.heightmap = "ramp.png";
    const auto data = terrainData(t, dir.path());
    EXPECT_NEAR(data->heightAt(-5.0f, 0.0f), 0.0f, 0.01f);
    EXPECT_NEAR(data->heightAt(5.0f, 0.0f), 4.0f, 0.01f);
    EXPECT_NEAR(data->heightAt(0.0f, 0.0f), 2.0f, 0.15f);
    const glm::vec3 n = data->normalAt(0.0f, 0.0f);
    EXPECT_LT(n.x, -0.3f) << "slopes up towards +X";
}

TEST(Terrain, BallsRollDownItAndAgentsWalkOnIt) {
    test::TempDir dir;
    std::vector<std::uint8_t> pixels(16 * 16, 0);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) pixels[static_cast<std::size_t>(y * 16 + x)] = static_cast<std::uint8_t>(x * 255 / 15);
    }
    ASSERT_NE(stbi_write_png((dir.path() / "slope.png").string().c_str(), 16, 16, 1, pixels.data(), 16), 0);
    Scene scene;
    auto& reg = scene.registry();
    const entt::entity ground = scene.createEntity("Ground");
    auto& t = reg.emplace<TerrainComponent>(ground);
    t.size = 40.0f;
    t.height = 8.0f;  // a 1:5 slope rising to +X
    t.resolution = 33;
    t.heightmap = "slope.png";
    const entt::entity ball = scene.createEntity("Ball");
    auto& c = reg.emplace<ColliderComponent>(ball);
    c.shape = ColliderShape::Sphere;
    c.radius = 0.5f;
    reg.emplace<RigidBodyComponent>(ball);
    reg.get<TransformComponent>(ball).position = {10.0f, 8.0f, 0.0f};
    scene.updateWorldTransforms();
    Simulation sim(scene);
    sim.setProjectRoot(dir.path());
    ASSERT_NE(sim.physics(), nullptr);
    EXPECT_EQ(sim.physics()->terrainCount(), 1u);
    for (int i = 0; i < 180; ++i) sim.step();
    const glm::vec3 p = reg.get<TransformComponent>(ball).position;
    EXPECT_LT(p.x, 9.0f) << "rolls downhill (-X)";
    const float ground_y = terrainData(t, dir.path())->heightAt(p.x, p.z);
    EXPECT_GT(p.y, ground_y + 0.3f) << "on the surface, not through it";

    NavigationWorld nav(scene);
    nav.setProjectRoot(dir.path());
    ASSERT_TRUE(nav.build().has_value());
    const auto path = nav.findPath({-15.0f, 1.0f, 0.0f}, {15.0f, 7.0f, 0.0f});
    ASSERT_GE(path.size(), 2u) << "the terrain is walkable";
    EXPECT_NEAR(path.back().y, terrainData(t, dir.path())->heightAt(15.0f, 0.0f), 0.6f);
}

TEST(Foliage, ScattersOnTheTerrainAndAvoidsSteepGround) {
    Scene scene;
    auto& reg = scene.registry();
    const entt::entity ground = scene.createEntity("Ground");
    auto& t = reg.emplace<TerrainComponent>(ground);
    t.size = 60.0f;
    t.height = 6.0f;
    t.resolution = 65;
    reg.get<TransformComponent>(ground).position.y = 2.0f;
    const entt::entity grass = scene.createEntity("Grass");
    auto& f = reg.emplace<FoliageComponent>(grass);
    f.count = 300;
    f.radius = 20.0f;
    f.maxSlope = 90.0f;
    scene.updateWorldTransforms();
    const auto copies = foliageInstances(scene, grass, {});
    ASSERT_EQ(copies.size(), 300u);
    for (const glm::mat4& m : copies) {
        const glm::vec3 p(m[3]);
        ASSERT_LE(std::abs(p.x), 20.0f);
        const auto data = terrainData(t, {});
        ASSERT_NEAR(p.y, 2.0f + data->heightAt(p.x, p.z), 0.01f) << "standing on the ground";
    }
    EXPECT_EQ(foliageInstances(scene, grass, {}), copies) << "same seed, same places";
    const std::uint64_t key = foliageKey(scene, grass, {});
    f.maxSlope = 2.0f;  // almost only flat ground
    EXPECT_NE(foliageKey(scene, grass, {}), key);
    EXPECT_LT(foliageInstances(scene, grass, {}).size(), 300u) << "steeper spots stay bare";
}

TEST(Water, DynamicBodiesFloat) {
    Scene scene;
    auto& reg = scene.registry();
    const entt::entity pool = scene.createEntity("Pool");
    reg.emplace<WaterComponent>(pool).buoyancy = 1.5f;
    const entt::entity crate = scene.createEntity("Crate");
    reg.emplace<ColliderComponent>(crate);  // 1 m cube
    reg.emplace<RigidBodyComponent>(crate).mass = 400.0f;  // lighter than water
    reg.get<TransformComponent>(crate).position = {0.0f, 3.0f, 0.0f};
    const entt::entity rock = scene.createEntity("Rock");
    reg.emplace<ColliderComponent>(rock);
    reg.emplace<RigidBodyComponent>(rock).mass = 400.0f;
    reg.get<TransformComponent>(rock).position = {200.0f, 3.0f, 0.0f};  // outside the water
    scene.updateWorldTransforms();
    Simulation sim(scene);
    for (int i = 0; i < 60 * 6; ++i) sim.step();
    const float y = reg.get<TransformComponent>(crate).position.y;
    EXPECT_GT(y, -0.6f) << "floats at the surface";
    EXPECT_LT(y, 0.6f);
    EXPECT_LT(reg.get<TransformComponent>(rock).position.y, -50.0f) << "no water there: falls";
}

TEST(Landscape, ComponentsSerialize) {
    Scene scene;
    const entt::entity e = scene.createEntity("E");
    scene.registry().emplace<TerrainComponent>(e).heightmap = "Content/h.png";
    scene.registry().emplace<WaterComponent>(e).clarity = 9.0f;
    scene.registry().emplace<FoliageComponent>(e).count = 1234;
    auto cloned = SceneSerializer::clone(scene);
    ASSERT_TRUE(cloned.has_value());
    const entt::entity c = (*cloned)->findByUuid(scene.uuidOf(e));
    EXPECT_EQ((*cloned)->registry().get<TerrainComponent>(c).heightmap, "Content/h.png");
    EXPECT_FLOAT_EQ((*cloned)->registry().get<WaterComponent>(c).clarity, 9.0f);
    EXPECT_EQ((*cloned)->registry().get<FoliageComponent>(c).count, 1234);
}
