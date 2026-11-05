// ScenePatch: per-entity scene differences used by undo/redo and collaboration.
#include "scene/Scene.hpp"
#include "scene/ScenePatch.hpp"
#include "scene/SceneSerializer.hpp"

#include <gtest/gtest.h>

using namespace lumora;
using nlohmann::json;

namespace {

entt::entity add(Scene& scene, const char* name, entt::entity parent = entt::null) {
    const auto e = scene.createEntity(name, Uuid::generate(), parent);
    scene.registry().get<TransformComponent>(e).position = {1.0f, 2.0f, 3.0f};
    return e;
}

std::unique_ptr<Scene> sample() {
    auto scene = std::make_unique<Scene>("Sample");
    const auto a = add(*scene, "A");
    add(*scene, "A1", a);
    add(*scene, "A2", a);
    const auto b = add(*scene, "B");
    scene->registry().emplace<MeshRendererComponent>(b).baseColor = {0.2f, 0.4f, 0.6f};
    add(*scene, "C");
    scene->updateWorldTransforms();
    return scene;
}

entt::entity named(const Scene& scene, std::string_view name) {
    entt::entity found = entt::null;
    scene.forEachDepthFirst([&](entt::entity e, int) {
        if (scene.nameOf(e) == name) found = e;
    });
    return found;
}

std::unique_ptr<Scene> copyOf(const Scene& scene) { return std::move(*SceneSerializer::clone(scene)); }

} // namespace

TEST(ScenePatch, IdenticalScenesGiveAnEmptyPatch) {
    auto scene = sample();
    const SceneIndex index = SceneIndex::fromScene(*scene);
    EXPECT_TRUE(ScenePatch::between(index, index).empty());
}

TEST(ScenePatch, ReproducesEveryKindOfEdit) {
    auto from = sample();
    auto to = copyOf(*from);
    Scene& s = *to;
    s.destroyEntity(named(s, "A2"));                                       // removal
    s.rename(named(s, "B"), "B renamed");                                  // rename
    s.registry().remove<MeshRendererComponent>(named(s, "B renamed"));     // component removed
    s.registry().emplace<LightComponent>(named(s, "C")).intensity = 7.0f;  // component added
    s.registry().get<TransformComponent>(named(s, "A1")).scale = glm::vec3(4.0f);
    s.setParent(named(s, "C"), named(s, "A"), 0);                          // reparent + order
    s.setParent(named(s, "A"), entt::null, 1);                             // root reorder
    add(s, "New", named(s, "C"));                                          // creation (nested)
    s.environment().fogDensity = 0.123f;
    s.setName("Renamed scene");
    s.updateWorldTransforms();

    const ScenePatch patch = ScenePatch::between(SceneIndex::fromScene(*from), SceneIndex::fromScene(*to));
    EXPECT_FALSE(patch.empty());
    ASSERT_TRUE(patch.applyTo(*from).has_value());
    EXPECT_EQ(SceneSerializer::toJson(*from), SceneSerializer::toJson(*to));
}

TEST(ScenePatch, SurvivesTheWireFormat) {
    auto from = sample();
    auto to = copyOf(*from);
    to->destroyEntity(named(*to, "C"));
    add(*to, "D");
    const ScenePatch patch = ScenePatch::between(SceneIndex::fromScene(*from), SceneIndex::fromScene(*to));
    auto decoded = ScenePatch::fromJson(json::parse(patch.toJson().dump()));
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    ASSERT_TRUE(decoded->applyTo(*from).has_value());
    EXPECT_EQ(SceneSerializer::toJson(*from), SceneSerializer::toJson(*to));
}

TEST(ScenePatch, ReversingAnEditKeepsUnrelatedConcurrentChanges) {
    // What undo does in a shared session: revert only what the edit touched.
    auto scene = sample();
    const json before = SceneSerializer::toJson(*scene);
    add(*scene, "Mine");
    const json after = SceneSerializer::toJson(*scene);
    // A collaborator meanwhile edits another entity.
    scene->registry().get<TransformComponent>(named(*scene, "B")).position = {9.0f, 9.0f, 9.0f};

    const ScenePatch undo = ScenePatch::between(SceneIndex::fromDocument(after), SceneIndex::fromDocument(before));
    ASSERT_TRUE(undo.applyTo(*scene).has_value());
    EXPECT_EQ(named(*scene, "Mine"), entt::entity{entt::null});
    EXPECT_EQ(scene->registry().get<TransformComponent>(named(*scene, "B")).position, glm::vec3(9.0f));
}

TEST(ScenePatch, OrphanedRecordsAttachToTheRoot) {
    Scene scene;
    ScenePatch patch;
    const Uuid child = Uuid::generate();
    patch.upserts.push_back(json{{"uuid", child.toString()},
                                 {"name", "Orphan"},
                                 {"parent", Uuid::generate().toString()},
                                 {"components", {{"Transform", json::object()}}}});
    ASSERT_TRUE(patch.applyTo(scene).has_value());
    const entt::entity e = scene.findByUuid(child);
    ASSERT_NE(e, entt::entity{entt::null});
    EXPECT_EQ(scene.parentOf(e), entt::entity{entt::null});
}

TEST(ScenePatch, RejectsMalformedInput) {
    EXPECT_FALSE(ScenePatch::fromJson(json::array()).has_value());
    EXPECT_FALSE(ScenePatch::fromJson(json{{"upserts", json::object()}}).has_value());
    EXPECT_FALSE(ScenePatch::fromJson(json{{"upserts", json::array({json{{"name", "x"}}})}}).has_value());
    EXPECT_FALSE(ScenePatch::fromJson(json{{"removals", json::array({"not-a-uuid"})}}).has_value());
    EXPECT_FALSE(ScenePatch::fromJson(json{{"environment", 3}}).has_value());

    // Invalid records inside an otherwise valid patch are skipped, the rest still applies.
    Scene scene;
    ScenePatch patch;
    patch.upserts.push_back(json{{"uuid", "garbage"}});
    patch.upserts.push_back(json{{"uuid", Uuid::generate().toString()}, {"name", "Ok"}});
    EXPECT_FALSE(patch.applyTo(scene).has_value());
    EXPECT_EQ(scene.entityCount(), 1u);
}

TEST(ScenePatch, MovingUnderOwnDescendantIsRefused) {
    auto scene = sample();
    auto target = copyOf(*scene);
    // Craft a record that puts A under its child A1.
    json record = SceneSerializer::entityToJson(*target, named(*target, "A"));
    record["parent"] = target->uuidOf(named(*target, "A1")).toString();
    ScenePatch patch;
    patch.upserts.push_back(record);
    EXPECT_FALSE(patch.applyTo(*scene).has_value());
    EXPECT_EQ(scene->parentOf(named(*scene, "A")), entt::entity{entt::null});
}
