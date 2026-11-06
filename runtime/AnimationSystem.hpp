#pragma once

#include <entt/entity/entity.hpp>

#include <filesystem>
#include <memory>
#include <string_view>

namespace lumora {

class Scene;

/// Plays glTF node animations (AnimatorComponent) during the simulation: every step it advances
/// each animator's clock, samples its clip and writes the animated translation, rotation and
/// scale into the entities mirroring the model's nodes (ModelNodeComponent below the animator).
/// Properties the clip does not animate keep whatever the scene (or a script) set.
class AnimationSystem {
public:
    AnimationSystem(Scene& scene, std::filesystem::path projectRoot);
    ~AnimationSystem();
    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    void update(float dt);
    /// Starts a clip (by name; empty = the first) from the beginning. False if there is none.
    bool play(entt::entity animator, std::string_view clip);
    /// Starts a clip and blends into it from the current pose over `seconds` (the old clip keeps
    /// moving while it fades out): walk to run, idle to jump without a pop.
    bool crossfade(entt::entity animator, std::string_view clip, float seconds);
    /// 0..1 while a crossfade runs, 1 when the current clip plays alone.
    [[nodiscard]] float blendWeight(entt::entity animator) const;
    void stop(entt::entity animator);
    [[nodiscard]] bool isPlaying(entt::entity animator) const;
    /// Seconds into the current clip.
    [[nodiscard]] float time(entt::entity animator) const;

    [[nodiscard]] static bool sceneHasAnimators(const Scene& scene);

private:
    bool start(entt::entity animator, std::string_view clip);  // play, keeping a crossfade's source
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lumora
