#include "TestUtils.hpp"

#include "scene/ComponentRegistry.hpp"
#include "scene/SceneSerializer.hpp"

#include <nlohmann/json.hpp>

using namespace lumora;

namespace {
std::unique_ptr<Scene> makeSampleScene() {
    auto scene = std::make_unique<Scene>("Sample");
    scene->environment().exposure = 1.7f;
    const auto root = scene->createEntity("Root");
    auto& t = scene->registry().get<TransformComponent>(root);
    t.position = {1, 2, 3};
    t.rotationDeg = {10, 20, 30};
    t.scale = {2, 2, 2};
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(root);
    mesh.mesh = MeshPrimitive::Sphere;
    mesh.baseColor = {0.1f, 0.2f, 0.3f};
    mesh.metallic = 1.0f;
    mesh.roughness = 0.25f;
    const auto child = scene->createEntity("Light", Uuid::generate(), root);
    auto& light = scene->registry().emplace<LightComponent>(child);
    light.type = LightType::Point;
    light.intensity = 42.0f;
    (void)scene->createEntity("Second Root");
    return scene;
}
} // namespace

TEST(SceneSerializer, RoundTripPreservesEverything) {
    const auto original = makeSampleScene();
    const std::string text = SceneSerializer::toString(*original);
    auto loaded = SceneSerializer::fromString(text);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    Scene& scene = **loaded;

    EXPECT_EQ(scene.name(), "Sample");
    EXPECT_FLOAT_EQ(scene.environment().exposure, 1.7f);
    EXPECT_EQ(scene.entityCount(), 3u);
    ASSERT_EQ(scene.roots().size(), 2u);
    const entt::entity root = scene.roots()[0];
    EXPECT_EQ(scene.uuidOf(root), original->uuidOf(original->roots()[0]));
    EXPECT_EQ(scene.nameOf(scene.roots()[1]), "Second Root");

    const auto& t = scene.registry().get<TransformComponent>(root);
    EXPECT_EQ(t.position, glm::vec3(1, 2, 3));
    EXPECT_EQ(t.rotationDeg, glm::vec3(10, 20, 30));
    const auto& mesh = scene.registry().get<MeshRendererComponent>(root);
    EXPECT_EQ(mesh.mesh, MeshPrimitive::Sphere);
    EXPECT_FLOAT_EQ(mesh.roughness, 0.25f);

    ASSERT_EQ(scene.childrenOf(root).size(), 1u);
    const entt::entity child = scene.childrenOf(root)[0];
    const auto& light = scene.registry().get<LightComponent>(child);
    EXPECT_EQ(light.type, LightType::Point);
    EXPECT_FLOAT_EQ(light.intensity, 42.0f);

    // Serializing again must be byte-identical (stable ordering).
    EXPECT_EQ(SceneSerializer::toString(scene), text);
}

TEST(SceneSerializer, SaveAndLoadFile) {
    test::TempDir dir;
    const auto original = makeSampleScene();
    const auto path = dir.path() / "Scenes" / "Test.lscene";
    ASSERT_TRUE(SceneSerializer::save(*original, path).has_value());
    auto loaded = SceneSerializer::load(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->entityCount(), original->entityCount());
}

TEST(SceneSerializer, RejectsInvalidInput) {
    EXPECT_FALSE(SceneSerializer::fromString("not json").has_value());
    EXPECT_FALSE(SceneSerializer::fromString(R"({"format":"other"})").has_value());
    EXPECT_FALSE(SceneSerializer::fromString(R"({"format":"lumora.scene","version":999,"entities":[]})").has_value());
    EXPECT_FALSE(SceneSerializer::fromString(
                     R"({"format":"lumora.scene","version":1,"entities":[{"uuid":"1","parent":"2"}]})")
                     .has_value());
}

TEST(SceneSerializer, ToleratesUnknownComponentsAndFields) {
    const char* text = R"({
        "format": "lumora.scene", "version": 1, "name": "X",
        "entities": [{
            "uuid": "00000000000000ab", "name": "E",
            "components": {
                "FutureComponent": {"a": 1},
                "Transform": {"position": [1, 2, 3], "unknownField": true, "scale": "bad"}
            }
        }]
    })";
    auto loaded = SceneSerializer::fromString(text);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    const entt::entity e = (*loaded)->findByUuid(Uuid(0xab));
    ASSERT_NE(e, entt::entity{entt::null});
    const auto& t = (*loaded)->registry().get<TransformComponent>(e);
    EXPECT_EQ(t.position, glm::vec3(1, 2, 3));
    EXPECT_EQ(t.scale, glm::vec3(1, 1, 1)) << "invalid value keeps default";
}

TEST(ComponentRegistry, BuiltinFieldsRoundTripThroughAccessors) {
    Scene scene;
    const auto e = scene.createEntity("E");
    const ComponentDesc* light = ComponentRegistry::builtin().find("Light");
    ASSERT_NE(light, nullptr);
    EXPECT_FALSE(light->has(scene.registry(), e));
    light->add(scene.registry(), e);
    ASSERT_TRUE(light->has(scene.registry(), e));
    const FieldDesc* type = light->findField("type");
    ASSERT_NE(type, nullptr);
    type->set(scene.registry(), e, FieldValue(1));
    EXPECT_EQ(scene.registry().get<LightComponent>(e).type, LightType::Point);
    EXPECT_EQ(std::get<int>(type->get(scene.registry(), e)), 1);
    light->remove(scene.registry(), e);
    EXPECT_FALSE(light->has(scene.registry(), e));
    const ComponentDesc* transform = ComponentRegistry::builtin().find("Transform");
    ASSERT_NE(transform, nullptr);
    EXPECT_FALSE(transform->removable);
}
