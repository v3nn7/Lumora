#pragma once

#include <entt/entity/entity.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lumora {

class AnimationSystem;
class AudioWorld;
class Scene;
struct TimelineAsset;

/// Plays the scene's Timeline components: moves and shows/hides their targets, starts
/// animations and sounds at their keys and collects events for scripts.
class TimelineSystem {
public:
    TimelineSystem(Scene& scene, std::filesystem::path projectRoot);
    ~TimelineSystem();

    void update(float dt, AnimationSystem* animation, AudioWorld* audio);
    /// Plays an entity's timeline from the start. False if it has none (or it cannot be read).
    bool play(entt::entity entity);
    void stop(entt::entity entity);
    [[nodiscard]] bool isPlaying(entt::entity entity) const;
    /// Seconds into the entity's timeline.
    [[nodiscard]] float time(entt::entity entity) const;
    /// Whether any timeline passed an event key with this name in the latest update.
    [[nodiscard]] bool eventFired(std::string_view name) const;

    [[nodiscard]] static bool sceneHasTimelines(const Scene& scene);

private:
    struct State {
        std::string file;
        std::shared_ptr<const TimelineAsset> asset;
        float time = 0.0f;
        bool playing = false;
        bool started = false;
    };
    bool bind(entt::entity entity, State& state);
    void apply(entt::entity owner, State& state, float from, float to, AnimationSystem* animation, AudioWorld* audio);

    Scene& scene_;
    std::filesystem::path root_;
    std::unordered_map<entt::entity, State> states_;
    std::vector<std::string> events_;
};

} // namespace lumora
