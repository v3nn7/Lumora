#include "TestUtils.hpp"

#include "runtime/Simulation.hpp"
#include "runtime/TimelineSystem.hpp"
#include "scene/Scene.hpp"
#include "scene/Timeline.hpp"

#include <gtest/gtest.h>

using namespace lumora;
using Kind = TimelineAsset::TrackKind;

namespace {

TimelineAsset::Key at(float time, glm::vec3 position, glm::vec3 rotation = glm::vec3(0.0f)) {
    TimelineAsset::Key k;
    k.time = time;
    k.position = position;
    k.rotationDeg = rotation;
    return k;
}

TimelineAsset cutscene() {
    TimelineAsset t;
    t.duration = 2.0f;
    auto& camera = t.track(Kind::Transform, "Camera");
    TimelineAsset::setKey(camera, at(1.0f, {10, 0, 0}, {0, 90, 0}));
    TimelineAsset::setKey(camera, at(0.0f, {0, 0, 0}));  // inserted before: kept sorted
    TimelineAsset::Key hide;
    hide.time = 1.5f;
    hide.visible = false;
    TimelineAsset::setKey(t.track(Kind::Visible, "Door"), hide);
    TimelineAsset::Key boom;
    boom.time = 0.5f;
    boom.text = "boom";
    TimelineAsset::setKey(t.track(Kind::Event, ""), boom);
    return t;
}

} // namespace

TEST(Timeline, KeysEvaluateAndRoundTrip) {
    TimelineAsset t = cutscene();
    const auto& camera = t.tracks[0];
    ASSERT_EQ(camera.keys.size(), 2u);
    EXPECT_FLOAT_EQ(camera.keys[0].time, 0.0f);
    EXPECT_NEAR(evaluateTransform(camera, -1.0f)->position.x, 0.0f, 1e-5f) << "held before the first key";
    EXPECT_NEAR(evaluateTransform(camera, 5.0f)->position.x, 10.0f, 1e-5f) << "held after the last";
    const auto mid = evaluateTransform(camera, 0.5f);
    EXPECT_NEAR(mid->position.x, 5.0f, 0.01f);
    EXPECT_NEAR(mid->rotationDeg.y, 45.0f, 0.5f) << "rotation turns halfway";
    TimelineAsset::setKey(t.tracks[0], at(1.0f, {20, 0, 0}));
    EXPECT_EQ(t.tracks[0].keys.size(), 2u) << "a key at the same time is replaced";
    EXPECT_EQ(evaluateVisible(t.tracks[1], 1.0f), std::nullopt) << "nothing before the first key";
    EXPECT_EQ(evaluateVisible(t.tracks[1], 1.6f), false);
    EXPECT_EQ(keysBetween(t.tracks[2], 0.0f, 0.5f).size(), 1u);
    EXPECT_TRUE(keysBetween(t.tracks[2], 0.5f, 1.0f).empty()) << "(from, to]: fires once";

    test::TempDir dir;
    ASSERT_TRUE(saveTimeline(t, dir.path() / "a.ltimeline").has_value());
    auto loaded = loadTimeline(dir.path() / "a.ltimeline");
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    ASSERT_EQ(loaded->tracks.size(), 3u);
    EXPECT_EQ(loaded->tracks[0].target, "Camera");
    EXPECT_FLOAT_EQ(loaded->tracks[0].keys[1].position.x, 20.0f);
    EXPECT_FALSE(loaded->tracks[1].keys[0].visible);
    EXPECT_EQ(loaded->tracks[2].keys[0].text, "boom");
    EXPECT_FLOAT_EQ(loaded->duration, 2.0f);
}

TEST(Timeline, PlaysInTheSimulation) {
    test::TempDir dir;
    ASSERT_TRUE(saveTimeline(cutscene(), dir.path() / "Intro.ltimeline").has_value());
    Scene scene;
    auto& reg = scene.registry();
    const entt::entity camera = scene.createEntity("Camera");
    const entt::entity door = scene.createEntity("Door");
    reg.emplace<MeshRendererComponent>(door);
    const entt::entity director = scene.createEntity("Director");
    reg.emplace<TimelineComponent>(director).timeline = "Intro.ltimeline";
    Simulation sim(scene);
    sim.setProjectRoot(dir.path());
    bool boom = false;
    for (int i = 0; i < 30; ++i) {  // 0.5 s
        sim.step();
        boom = boom || sim.timelines().eventFired("boom");
    }
    EXPECT_TRUE(boom) << "the event fired";
    EXPECT_NEAR(reg.get<TransformComponent>(camera).position.x, 5.0f, 0.3f);
    EXPECT_TRUE(sim.timelines().isPlaying(director));
    for (int i = 0; i < 100; ++i) sim.step();  // past the end
    EXPECT_NEAR(reg.get<TransformComponent>(camera).position.x, 10.0f, 1e-3f);
    EXPECT_NEAR(reg.get<TransformComponent>(camera).rotationDeg.y, 90.0f, 0.5f);
    EXPECT_FALSE(reg.get<MeshRendererComponent>(door).visible) << "hidden at 1.5 s";
    EXPECT_FALSE(sim.timelines().isPlaying(director)) << "stops at the end";
    EXPECT_FLOAT_EQ(sim.timelines().time(director), 2.0f);

    ASSERT_TRUE(sim.timelines().play(director));
    EXPECT_NEAR(reg.get<TransformComponent>(camera).position.x, 0.0f, 1e-3f) << "from the start again";
    reg.get<TimelineComponent>(director).loop = true;
    for (int i = 0; i < 150; ++i) sim.step();  // 2.5 s: around once
    EXPECT_TRUE(sim.timelines().isPlaying(director));
    EXPECT_NEAR(sim.timelines().time(director), 0.5f, 0.02f);
}
