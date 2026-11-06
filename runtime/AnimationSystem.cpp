#include "runtime/AnimationSystem.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumora {

struct AnimationSystem::Impl {
    Scene& scene;
    AssetLibrary assets;
    struct State {
        const ImportedModel* model = nullptr;
        const ImportedAnimation* clip = nullptr;
        std::string clipName;  // as requested, to notice edits
        std::string modelKey;
        float time = 0.0f;
        bool playing = false;
        bool started = false;
        bool failed = false;
        std::vector<entt::entity> nodeEntities;  // by model node index
        std::vector<NodePose> poses;
        std::vector<std::uint8_t> animated;  // per node: bit0 translation, bit1 rotation, bit2 scale
    };
    std::unordered_map<entt::entity, State> states;

    Impl(Scene& s, std::filesystem::path root) : scene(s), assets(std::move(root)) {}

    void collectNodes(State& state, entt::entity e) {
        for (entt::entity child : scene.childrenOf(e)) {
            if (const auto* node = scene.registry().try_get<ModelNodeComponent>(child)) {
                if (node->node >= 0 && static_cast<std::size_t>(node->node) < state.nodeEntities.size()) {
                    state.nodeEntities[static_cast<std::size_t>(node->node)] = child;
                }
            }
            collectNodes(state, child);
        }
    }

    /// (Re)binds an animator to its model and clip; false if they cannot be used.
    bool bind(entt::entity e, const AnimatorComponent& animator, State& state) {
        if (state.failed && state.modelKey == animator.model && state.clipName == animator.clip) return false;
        state.failed = true;
        state.modelKey = animator.model;
        state.clipName = animator.clip;
        auto file = assets.filePath(animator.model);
        if (!file) return false;
        auto model = assets.model(*file);
        if (!model) {
            log::warn("Animation", "{}", model.error().message);
            return false;
        }
        state.model = *model;
        state.clip = nullptr;
        for (const ImportedAnimation& a : state.model->animations) {
            if (animator.clip.empty() || a.name == animator.clip) {
                state.clip = &a;
                break;
            }
        }
        if (state.clip == nullptr) {
            log::warn("Animation", "'{}' has no animation named '{}'", animator.model, animator.clip);
            return false;
        }
        const std::size_t nodes = state.model->nodes.size();
        state.nodeEntities.assign(nodes, entt::null);
        collectNodes(state, e);
        state.poses.assign(nodes, NodePose{});
        state.animated.assign(nodes, 0);
        for (const AnimationChannel& ch : state.clip->channels) {
            if (ch.node < 0 || static_cast<std::size_t>(ch.node) >= nodes) continue;
            state.animated[static_cast<std::size_t>(ch.node)] |= static_cast<std::uint8_t>(1u << static_cast<int>(ch.path));
        }
        state.failed = false;
        return true;
    }

    void apply(State& state) {
        sampleAnimation(*state.clip, state.time, state.poses);
        auto& reg = scene.registry();
        for (std::size_t i = 0; i < state.nodeEntities.size(); ++i) {
            const entt::entity e = state.nodeEntities[i];
            if (state.animated[i] == 0 || e == entt::null || !reg.valid(e)) continue;
            auto& t = reg.get<TransformComponent>(e);
            const NodePose& pose = state.poses[i];
            if (state.animated[i] & 1u) t.position = pose.translation;
            if (state.animated[i] & 2u) {
                float y = 0.0f, x = 0.0f, z = 0.0f;
                glm::extractEulerAngleYXZ(glm::mat4_cast(pose.rotation), y, x, z);
                t.rotationDeg = glm::degrees(glm::vec3(x, y, z));
            }
            if (state.animated[i] & 4u) t.scale = pose.scale;
        }
    }
};

AnimationSystem::AnimationSystem(Scene& scene, std::filesystem::path projectRoot)
    : impl_(std::make_unique<Impl>(scene, std::move(projectRoot))) {}
AnimationSystem::~AnimationSystem() = default;

bool AnimationSystem::sceneHasAnimators(const Scene& scene) {
    return !scene.registry().view<AnimatorComponent>().empty();
}

void AnimationSystem::update(float dt) {
    Impl& w = *impl_;
    auto& reg = w.scene.registry();
    std::erase_if(w.states, [&](const auto& entry) { return !reg.valid(entry.first) || !reg.all_of<AnimatorComponent>(entry.first); });
    for (auto [e, animator] : reg.view<AnimatorComponent>().each()) {
        Impl::State& state = w.states[e];
        if (state.clip == nullptr || state.modelKey != animator.model || state.clipName != animator.clip) {
            if (!w.bind(e, animator, state)) continue;
            if (!state.started) {
                state.started = true;
                state.playing = animator.playOnStart;
            }
        }
        if (!state.playing) continue;
        state.time += dt * animator.speed;
        const float duration = state.clip->duration;
        if (duration > 0.0f) {
            if (animator.loop) {
                state.time = std::fmod(state.time, duration);
                if (state.time < 0.0f) state.time += duration;
            } else if (state.time >= duration || state.time <= 0.0f) {
                state.time = std::clamp(state.time, 0.0f, duration);
                state.playing = false;  // ends on its last (or, backwards, first) pose
            }
        }
        w.apply(state);
    }
}

bool AnimationSystem::play(entt::entity animator, std::string_view clip) {
    Impl& w = *impl_;
    auto* component = w.scene.registry().try_get<AnimatorComponent>(animator);
    if (component == nullptr) return false;
    if (!clip.empty()) component->clip = std::string(clip);
    Impl::State& state = w.states[animator];
    if (!w.bind(animator, *component, state)) return false;
    state.started = true;
    state.playing = true;
    state.time = component->speed < 0.0f ? state.clip->duration : 0.0f;
    w.apply(state);
    return true;
}

void AnimationSystem::stop(entt::entity animator) {
    if (auto it = impl_->states.find(animator); it != impl_->states.end()) it->second.playing = false;
}

bool AnimationSystem::isPlaying(entt::entity animator) const {
    const auto it = impl_->states.find(animator);
    return it != impl_->states.end() && it->second.playing;
}

float AnimationSystem::time(entt::entity animator) const {
    const auto it = impl_->states.find(animator);
    return it != impl_->states.end() ? it->second.time : 0.0f;
}

} // namespace lumora
