#pragma once

#include "core/Error.hpp"

#include <entt/entity/entity.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace lumora {

class Scene;

/// Sound for a running simulation (miniaudio): one voice per entity with an AudioSourceComponent,
/// placed at the entity in 3D (or played flat), heard from the listener (the camera).
///
/// update() follows the scene every step: it creates voices for new sources (starting those with
/// playOnStart), moves 3D voices with their entities and drops voices of removed entities.
/// Clips are project-relative files (WAV, FLAC or MP3); long ones stream from disk.
class AudioWorld {
public:
    struct Options {
        std::filesystem::path projectRoot;
        /// false: no audio device; the mix is only advanced by mix() (headless tools, tests).
        bool device = true;
    };
    [[nodiscard]] static Result<std::unique_ptr<AudioWorld>> create(Scene& scene, const Options& options);
    ~AudioWorld();
    AudioWorld(const AudioWorld&) = delete;
    AudioWorld& operator=(const AudioWorld&) = delete;

    void update();
    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    /// Places the listener at a camera (its view matrix).
    void setListenerFromView(const glm::mat4& view);
    /// Pauses or resumes every sound (play mode paused).
    void setPaused(bool paused);

    /// Starts (from the beginning) or stops an entity's sound. False if it has none.
    bool play(entt::entity entity);
    bool stop(entt::entity entity);
    [[nodiscard]] bool isPlaying(entt::entity entity) const;
    /// Seconds of the entity's clip played so far (0 if none).
    [[nodiscard]] float cursorSeconds(entt::entity entity) const;
    [[nodiscard]] std::size_t voiceCount() const;
    /// Advances the mix by `frames` without a device; returns the peak sample (for tests).
    float mix(std::uint32_t frames);

    /// True if the scene has anything to play.
    [[nodiscard]] static bool sceneHasAudio(const Scene& scene);

private:
    struct Impl;
    explicit AudioWorld(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace lumora
