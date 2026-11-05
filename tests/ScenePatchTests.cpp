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
                                 {"full", true},
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
    patch.upserts.push_back(json{{"uuid", Uuid::generate().toString()}, {"full", true}, {"name", "Ok"}});
    EXPECT_FALSE(patch.applyTo(scene).has_value());
    EXPECT_EQ(scene.entityCount(), 1u);
}

TEST(ScenePatch, MovingUnderOwnDescendantIsRefused) {
    auto scene = sample();
    auto target = copyOf(*scene);
    // Craft a move that puts A under its child A1.
    ScenePatch patch;
    patch.upserts.push_back(json{{"uuid", target->uuidOf(named(*target, "A")).toString()},
                                 {"move", {{"parent", target->uuidOf(named(*target, "A1")).toString()}, {"index", 0}}}});
    EXPECT_FALSE(patch.applyTo(*scene).has_value());
    EXPECT_EQ(scene->parentOf(named(*scene, "A")), entt::entity{entt::null});
}

TEST(ScenePatch, CarriesOnlyWhatChanged) {
    auto from = sample();
    auto to = copyOf(*from);
    to->destroyEntity(named(*to, "A"));  // its siblings shift, but are not rewritten
    to->registry().get<TransformComponent>(named(*to, "B")).position.x = 42.0f;
    const ScenePatch patch = ScenePatch::between(SceneIndex::fromScene(*from), SceneIndex::fromScene(*to));
    ASSERT_EQ(patch.upserts.size(), 1u);
    const json& record = patch.upserts[0];
    EXPECT_FALSE(record.contains("full"));
    EXPECT_FALSE(record.contains("move"));
    EXPECT_FALSE(record.contains("name"));
    ASSERT_TRUE(record.contains("components"));
    EXPECT_EQ(record["components"].size(), 1u);              // Transform only
    EXPECT_EQ(record["components"]["Transform"].size(), 1u);  // position only
    EXPECT_EQ(patch.removals.size(), 3u);                     // A and its two children
}

TEST(ScenePatch, ReorderMovesOnlyTheEntitiesThatMoved) {
    auto from = sample();
    auto to = copyOf(*from);
    to->setParent(named(*to, "C"), entt::null, 0);  // A B C -> C A B
    const ScenePatch patch = ScenePatch::between(SceneIndex::fromScene(*from), SceneIndex::fromScene(*to));
    ASSERT_EQ(patch.upserts.size(), 1u);
    EXPECT_TRUE(patch.upserts[0].contains("move"));
    ASSERT_TRUE(patch.applyTo(*from).has_value());
    EXPECT_EQ(SceneSerializer::toJson(*from), SceneSerializer::toJson(*to));
}

TEST(ScenePatch, DifferentFieldsOfOneEntityMerge) {
    auto base = sample();
    auto mine = copyOf(*base);
    auto theirs = copyOf(*base);
    mine->registry().get<TransformComponent>(named(*mine, "B")).position = {1.0f, 0.0f, 0.0f};
    theirs->registry().get<MeshRendererComponent>(named(*theirs, "B")).baseColor = {1.0f, 0.0f, 0.0f};
    const SceneIndex index = SceneIndex::fromScene(*base);
    const ScenePatch a = ScenePatch::between(index, SceneIndex::fromScene(*mine));
    const ScenePatch b = ScenePatch::between(index, SceneIndex::fromScene(*theirs));
    ASSERT_TRUE(a.applyTo(*base).has_value());
    ASSERT_TRUE(b.applyTo(*base).has_value());
    const entt::entity e = named(*base, "B");
    EXPECT_EQ(base->registry().get<TransformComponent>(e).position, glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(base->registry().get<MeshRendererComponent>(e).baseColor, glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST(ScenePatch, EditsOfDeletedEntitiesAreIgnored) {
    auto base = sample();
    auto edited = copyOf(*base);
    edited->registry().get<TransformComponent>(named(*edited, "C")).position = {5.0f, 5.0f, 5.0f};
    const ScenePatch edit = ScenePatch::between(SceneIndex::fromScene(*base), SceneIndex::fromScene(*edited));
    base->destroyEntity(named(*base, "C"));  // deleted elsewhere first
    ASSERT_TRUE(edit.applyTo(*base).has_value());
    EXPECT_EQ(named(*base, "C"), entt::entity{entt::null}) << "an edit must not resurrect a deleted entity";
}
