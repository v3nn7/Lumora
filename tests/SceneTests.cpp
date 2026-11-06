#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <fstream>

using namespace lumora;

TEST(Scene, CreateAndFindByUuid) {
    Scene scene;
    const Uuid id = Uuid::generate();
    const entt::entity e = scene.createEntity("Thing", id);
    EXPECT_EQ(scene.findByUuid(id), e);
    EXPECT_EQ(scene.uuidOf(e), id);
    EXPECT_EQ(scene.nameOf(e), "Thing");
    EXPECT_EQ(scene.entityCount(), 1u);
    EXPECT_EQ(scene.roots().size(), 1u);
}

TEST(Scene, DuplicateUuidIsReplaced) {
    Scene scene;
    const Uuid id = Uuid::generate();
    const entt::entity a = scene.createEntity("A", id);
    const entt::entity b = scene.createEntity("B", id);
    EXPECT_EQ(scene.findByUuid(id), a);
    EXPECT_NE(scene.uuidOf(b), id);
}

TEST(Scene, ParentingAndCyclePrevention) {
    Scene scene;
    const auto a = scene.createEntity("A");
    const auto b = scene.createEntity("B");
    const auto c = scene.createEntity("C");
    EXPECT_TRUE(scene.setParent(b, a));
    EXPECT_TRUE(scene.setParent(c, b));
    EXPECT_EQ(scene.roots().size(), 1u);
    EXPECT_FALSE(scene.setParent(a, c)) << "would create a cycle";
    EXPECT_FALSE(scene.setParent(a, a));
    EXPECT_TRUE(scene.isAncestor(a, c));
    EXPECT_EQ(scene.parentOf(c), b);
    EXPECT_TRUE(scene.setParent(c, entt::null));
    EXPECT_EQ(scene.roots().size(), 2u);
    EXPECT_TRUE(scene.childrenOf(b).empty());
}

TEST(Scene, DestroyRemovesSubtree) {
    Scene scene;
    const auto a = scene.createEntity("A");
    const auto b = scene.createEntity("B", Uuid::generate(), a);
    (void)scene.createEntity("C", Uuid::generate(), b);
    const auto d = scene.createEntity("D");
    scene.destroyEntity(a);
    EXPECT_EQ(scene.entityCount(), 1u);
    EXPECT_TRUE(scene.isValid(d));
    EXPECT_FALSE(scene.isValid(b));
    ASSERT_EQ(scene.roots().size(), 1u);
    EXPECT_EQ(scene.roots()[0], d);
}

TEST(Scene, WorldTransformsComposeParentChain) {
    Scene scene;
    const auto parent = scene.createEntity("Parent");
    const auto child = scene.createEntity("Child", Uuid::generate(), parent);
    auto& reg = scene.registry();
    reg.get<TransformComponent>(parent).position = {1, 0, 0};
    reg.get<TransformComponent>(parent).rotationDeg = {0, 90, 0};
    reg.get<TransformComponent>(child).position = {0, 0, -2};
    scene.updateWorldTransforms();
    const glm::vec3 worldPos = glm::vec3(reg.get<WorldTransformComponent>(child).matrix[3]);
    // Rotating (0,0,-2) by +90 deg yaw gives (-2,0,0); plus parent offset (1,0,0).
    EXPECT_NEAR(worldPos.x, -1.0f, 1e-5f);
    EXPECT_NEAR(worldPos.y, 0.0f, 1e-5f);
    EXPECT_NEAR(worldPos.z, 0.0f, 1e-5f);
    const glm::mat4 direct = scene.worldMatrix(child);
    EXPECT_NEAR(direct[3].x, -1.0f, 1e-5f);
}

TEST(Scene, DuplicateCopiesComponentsAndChildren) {
    Scene scene;
    const auto a = scene.createEntity("A");
    auto& mesh = scene.registry().emplace<MeshRendererComponent>(a);
    mesh.roughness = 0.123f;
    (void)scene.createEntity("Child", Uuid::generate(), a);
    const auto copy = scene.duplicateEntity(a);
    ASSERT_NE(copy, entt::entity{entt::null});
    EXPECT_NE(scene.uuidOf(copy), scene.uuidOf(a));
    ASSERT_TRUE(scene.registry().all_of<MeshRendererComponent>(copy));
    EXPECT_FLOAT_EQ(scene.registry().get<MeshRendererComponent>(copy).roughness, 0.123f);
    EXPECT_EQ(scene.childrenOf(copy).size(), 1u);
    ASSERT_EQ(scene.roots().size(), 2u);
    EXPECT_EQ(scene.roots()[1], copy);
    EXPECT_EQ(scene.entityCount(), 4u);
}

TEST(Scene, PrimaryCameraPrefersPrimaryFlag) {
    Scene scene;
    const auto a = scene.createEntity("A");
    scene.registry().emplace<CameraComponent>(a).primary = false;
    EXPECT_EQ(scene.primaryCamera(), a);
    const auto b = scene.createEntity("B");
    scene.registry().emplace<CameraComponent>(b).primary = true;
    EXPECT_EQ(scene.primaryCamera(), b);
}

#include "scene/Prefab.hpp"
#include "TestUtils.hpp"

TEST(Prefab, SavesAndInstantiatesASubtree) {
    Scene scene("Prefabs");
    const entt::entity car = scene.createEntity("Car");
    scene.registry().get<TransformComponent>(car).position = glm::vec3(1.0f, 2.0f, 3.0f);
    scene.registry().emplace<MeshRendererComponent>(car).baseColor = glm::vec3(1.0f, 0.0f, 0.0f);
    const entt::entity wheel = scene.createEntity("Wheel", Uuid::generate(), car);
    scene.registry().emplace<RotatorComponent>(wheel).speedDeg = 90.0f;
    (void)scene.createEntity("Hubcap", Uuid::generate(), wheel);
    scene.updateWorldTransforms();

    test::TempDir dir;
    const auto file = dir.path() / "Prefabs" / "Car.lprefab";
    ASSERT_TRUE(savePrefab(scene, car, file).has_value());
    EXPECT_TRUE(isPrefabFile(file));
    auto prefab = loadPrefab(file);
    ASSERT_TRUE(prefab.has_value()) << prefab.error().message;

    Scene target("Level");
    const entt::entity parking = target.createEntity("Parking");
    auto first = instantiatePrefab(target, *prefab, parking);
    auto second = instantiatePrefab(target, *prefab);
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(target.entityCount(), 1u + 2u * 3u);
    EXPECT_NE(target.uuidOf(*first), target.uuidOf(*second)) << "every instance has its own identity";
    EXPECT_NE(target.uuidOf(*first), scene.uuidOf(car));
    EXPECT_EQ(target.parentOf(*first), parking);
    EXPECT_TRUE(target.parentOf(*second) == entt::null);
    EXPECT_EQ(target.nameOf(*first), "Car");
    EXPECT_EQ(target.registry().get<TransformComponent>(*first).position, glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(target.registry().get<MeshRendererComponent>(*first).baseColor, glm::vec3(1.0f, 0.0f, 0.0f));
    ASSERT_EQ(target.childrenOf(*first).size(), 1u);
    const entt::entity copiedWheel = target.childrenOf(*first)[0];
    EXPECT_EQ(target.nameOf(copiedWheel), "Wheel");
    EXPECT_FLOAT_EQ(target.registry().get<RotatorComponent>(copiedWheel).speedDeg, 90.0f);
    ASSERT_EQ(target.childrenOf(copiedWheel).size(), 1u);
    EXPECT_EQ(target.nameOf(target.childrenOf(copiedWheel)[0]), "Hubcap");

    EXPECT_FALSE(loadPrefab(dir.path() / "missing.lprefab").has_value());
    const auto bogus = dir.path() / "bogus.lprefab";
    std::ofstream(bogus) << "{\"format\": \"something else\"}";
    EXPECT_FALSE(loadPrefab(bogus).has_value());
}
