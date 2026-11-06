// Audio: miniaudio without a device, mixed by hand so the output can be inspected.
#include "TestUtils.hpp"

#include "runtime/AudioWorld.hpp"
#include "runtime/Simulation.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <numbers>
#include <vector>

using namespace lumora;

namespace {

/// Mono 16-bit PCM WAV: a 440 Hz sine at full scale.
void writeTone(const std::filesystem::path& file, float seconds) {
    constexpr std::uint32_t kRate = 48000;
    const auto frames = static_cast<std::uint32_t>(seconds * kRate);
    std::vector<std::int16_t> samples(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        samples[i] = static_cast<std::int16_t>(32000.0 * std::sin(2.0 * std::numbers::pi * 440.0 * i / kRate));
    }
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t data = frames * 2;
    out.write("RIFF", 4);
    u32(36 + data);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);      // PCM
    u16(1);      // mono
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(data);
    out.write(reinterpret_cast<const char*>(samples.data()), data);
}

AudioSourceComponent source(const char* clip, bool playOnStart, bool spatial) {
    AudioSourceComponent s;
    s.clip = clip;
    s.playOnStart = playOnStart;
    s.spatial = spatial;
    return s;
}

} // namespace

TEST(Audio, PlaysStopsAndFollowsTheScene) {
    test::TempDir project;
    writeTone(project.path() / "Sounds" / "tone.wav", 1.0f);
    Scene scene("Audio");
    const entt::entity music = scene.createEntity("Music");
    scene.registry().emplace<AudioSourceComponent>(music, source("Sounds/tone.wav", true, false));
    const entt::entity button = scene.createEntity("Button");
    scene.registry().emplace<AudioSourceComponent>(button, source("Sounds/tone.wav", false, false));
    const entt::entity broken = scene.createEntity("Broken");
    scene.registry().emplace<AudioSourceComponent>(broken, source("Sounds/missing.wav", true, false));
    scene.updateWorldTransforms();
    ASSERT_TRUE(AudioWorld::sceneHasAudio(scene));

    auto audio = AudioWorld::create(scene, {project.path(), false});
    ASSERT_TRUE(audio.has_value()) << audio.error().message;
    AudioWorld* world = audio->get();
    Simulation sim(scene);
    sim.setAudio(std::move(*audio));
    sim.step();
    EXPECT_EQ(world->voiceCount(), 2u) << "the missing clip is reported, not fatal";
    EXPECT_TRUE(world->isPlaying(music));
    EXPECT_FALSE(world->isPlaying(button)) << "playOnStart off";
    EXPECT_GT(world->mix(4800), 0.5f) << "the tone reaches the mix";
    EXPECT_NEAR(world->cursorSeconds(music), 0.1f, 0.02f);

    EXPECT_TRUE(world->stop(music));
    EXPECT_TRUE(world->play(button));
    EXPECT_FALSE(world->isPlaying(music));
    EXPECT_TRUE(world->isPlaying(button));
    world->mix(48000 * 2);  // past the end of the 1 s clip
    EXPECT_FALSE(world->isPlaying(button)) << "a non-looping sound ends";

    scene.destroyEntity(music);
    sim.step();
    EXPECT_EQ(world->voiceCount(), 1u) << "removed entities drop their voice";
}

TEST(Audio, SpatialSoundsFadeWithDistance) {
    test::TempDir project;
    writeTone(project.path() / "tone.wav", 1.0f);
    auto peakAt = [&](float distance) {
        Scene scene("Audio");
        const entt::entity e = scene.createEntity("Emitter");
        scene.registry().get<TransformComponent>(e).position = glm::vec3(0.0f, 0.0f, -distance);
        auto& s = scene.registry().emplace<AudioSourceComponent>(e, source("tone.wav", true, true));
        s.minDistance = 1.0f;
        s.maxDistance = 100.0f;
        scene.updateWorldTransforms();
        auto audio = AudioWorld::create(scene, {project.path(), false});
        EXPECT_TRUE(audio.has_value());
        (*audio)->setListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        (*audio)->update();
        (*audio)->mix(2400);  // let the spatializer settle
        return (*audio)->mix(4800);
    };
    const float near = peakAt(1.0f);
    const float far = peakAt(20.0f);
    EXPECT_GT(near, 0.2f);
    EXPECT_LT(far, near * 0.25f) << "inverse attenuation: 20 m away is much quieter than 1 m";
}
