#include "runtime/AnimationSystem.hpp"

#include "runtime/AnimatorStateMachine.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
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
        std::vector<std::uint8_t> animated;  // per node: bit0 translation, bit1 rotation, bit2 scale, bit3 morph weights
        // Crossfade: the clip being left, still advancing, and how far the blend is.
        const ImportedAnimation* previous = nullptr;
        float previousTime = 0.0f;
        bool previousLoop = true;
        float blendDuration = 0.0f;
        float blendElapsed = 0.0f;
        std::vector<NodePose> previousPoses;
        // Animator controller (.lanim), when the component names one.
        std::optional<AnimatorStateMachine> machine;
        std::string controllerKey;
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
        markAnimated(state, *state.clip);
        if (state.previous != nullptr) markAnimated(state, *state.previous);
        state.failed = false;
        return true;
    }

    static void markAnimated(State& state, const ImportedAnimation& clip) {
        for (const AnimationChannel& ch : clip.channels) {
            if (ch.node < 0 || static_cast<std::size_t>(ch.node) >= state.animated.size()) continue;
            state.animated[static_cast<std::size_t>(ch.node)] |= static_cast<std::uint8_t>(1u << static_cast<int>(ch.path));
        }
    }

    /// Rest pose of every node, then the clip's channels on top.
    void samplePose(const State& state, const ImportedAnimation& clip, float time, std::vector<NodePose>& out) const {
        out.resize(state.model->nodes.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            const ImportedNode& n = state.model->nodes[i];
            out[i].translation = n.translation;
            out[i].rotation = n.rotation;
            out[i].scale = n.scale;
            out[i].weights.clear();
        }
        sampleAnimation(clip, time, out);
    }

    void apply(State& state) {
        samplePose(state, *state.clip, state.time, state.poses);
        if (state.previous != nullptr && state.blendDuration > 0.0f) {
            samplePose(state, *state.previous, state.previousTime, state.previousPoses);
            const float w = std::clamp(state.blendElapsed / state.blendDuration, 0.0f, 1.0f);
            for (std::size_t i = 0; i < state.poses.size(); ++i) {
                NodePose& p = state.poses[i];
                const NodePose& from = state.previousPoses[i];
                p.translation = glm::mix(from.translation, p.translation, w);
                p.rotation = glm::slerp(from.rotation, p.rotation, w);
                p.scale = glm::mix(from.scale, p.scale, w);
                if (!from.weights.empty() && from.weights.size() == p.weights.size()) {
                    for (std::size_t k = 0; k < p.weights.size(); ++k) p.weights[k] = from.weights[k] + (p.weights[k] - from.weights[k]) * w;
                }
            }
        }
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
            if ((state.animated[i] & 8u) && !pose.weights.empty()) {
                // Morph weights: the node's mesh, and its other material parts (children that
                // are not nodes themselves).
                auto write = [&](entt::entity target) {
                    if (auto* morph = reg.try_get<MorphWeightsComponent>(target)) morph->weights = pose.weights;
                };
                write(e);
                for (entt::entity child : scene.childrenOf(e)) {
                    if (!reg.all_of<ModelNodeComponent>(child)) write(child);
                }
            }
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
    std::vector<std::pair<entt::entity, AnimatorStateMachine::Change>> changes;
    for (auto [e, animator] : reg.view<AnimatorComponent>().each()) {
        Impl::State& state = w.states[e];
        if (state.controllerKey != animator.controller) {
            state.controllerKey = animator.controller;
            state.machine.reset();
            if (!animator.controller.empty()) {
                auto file = w.assets.filePath(animator.controller);
                auto machine = file ? AnimatorStateMachine::load(*file)
                                    : Result<AnimatorStateMachine>(std::unexpected(file.error()));
                if (machine) {
                    state.machine = std::move(*machine);
                    const AnimatorStateMachine::State* entry = state.machine->entry();
                    animator.clip = entry->clip;
                    animator.loop = entry->loop;
                    animator.speed = entry->speed;
                } else {
                    log::warn("Animation", "{}", machine.error().message);
                }
            }
        }
        if (state.clip == nullptr || state.modelKey != animator.model || state.clipName != animator.clip) {
            if (!w.bind(e, animator, state)) continue;
            if (!state.started) {
                state.started = true;
                state.playing = animator.playOnStart;
            }
        }
        if (!state.playing) continue;
        if (const auto* ragdoll = reg.try_get<RagdollComponent>(e); ragdoll != nullptr && ragdoll->active) continue;  // physics owns the bones
        if (state.previous != nullptr) {
            state.blendElapsed += dt;
            const float duration = state.previous->duration;
            state.previousTime += dt * animator.speed;
            if (duration > 0.0f) {
                state.previousTime = state.previousLoop ? std::fmod(std::max(state.previousTime, 0.0f), duration)
                                                        : std::clamp(state.previousTime, 0.0f, duration);
            }
            if (state.blendElapsed >= state.blendDuration) state.previous = nullptr;  // the new clip alone
        }
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
        if (state.machine) {
            const float length = state.clip->duration;
            const float normalized = length > 0.0f ? state.time / length : 1.0f;
            if (auto change = state.machine->update(normalized, !state.playing)) changes.emplace_back(e, *change);
        }
    }
    // State machine transitions, after the loop (crossfade() touches the components).
    for (const auto& [e, change] : changes) {
        auto& animator = reg.get<AnimatorComponent>(e);
        animator.speed = change.state->speed;
        if (change.fadeSeconds > 0.0f) {
            crossfade(e, change.state->clip, change.fadeSeconds);
        } else {
            play(e, change.state->clip);
        }
        animator.loop = change.state->loop;
    }
}

bool AnimationSystem::setParameter(entt::entity animator, int kind, std::string_view name, float value) {
    const auto it = impl_->states.find(animator);
    return it != impl_->states.end() && it->second.machine && it->second.machine->setParameter(name, kind, value);
}

std::string AnimationSystem::currentState(entt::entity animator) const {
    const auto it = impl_->states.find(animator);
    if (it == impl_->states.end() || !it->second.machine) return {};
    return it->second.machine->current()->name;
}

bool AnimationSystem::play(entt::entity animator, std::string_view clip) {
    if (auto it = impl_->states.find(animator); it != impl_->states.end()) it->second.previous = nullptr;
    return start(animator, clip);
}

bool AnimationSystem::start(entt::entity animator, std::string_view clip) {
    Impl& w = *impl_;
    auto* component = w.scene.registry().try_get<AnimatorComponent>(animator);
    if (component == nullptr) return false;
    if (!clip.empty()) component->clip = std::string(clip);
    Impl::State& state = w.states[animator];
    // Rebinding (a new clip) rebuilds which nodes are animated, the fading clip's ones included.
    state.clipName.clear();
    if (!w.bind(animator, *component, state)) return false;
    state.started = true;
    state.playing = true;
    state.time = component->speed < 0.0f ? state.clip->duration : 0.0f;
    w.apply(state);
    return true;
}

bool AnimationSystem::crossfade(entt::entity animator, std::string_view clip, float seconds) {
    Impl& w = *impl_;
    auto it = w.states.find(animator);
    const bool canBlend = seconds > 0.0f && it != w.states.end() && it->second.clip != nullptr && it->second.playing;
    const ImportedAnimation* from = canBlend ? it->second.clip : nullptr;
    const float fromTime = canBlend ? it->second.time : 0.0f;
    const auto* component = w.scene.registry().try_get<AnimatorComponent>(animator);
    const bool fromLoop = component != nullptr ? component->loop : true;
    if (canBlend) {
        // Bind sees the previous clip so its channels stay animated during the fade.
        it->second.previous = from;
    }
    if (!start(animator, clip)) {
        if (canBlend) it->second.previous = nullptr;
        return false;
    }
    Impl::State& state = w.states[animator];
    state.previous = from;
    state.previousTime = fromTime;
    state.previousLoop = fromLoop;
    state.blendDuration = canBlend ? seconds : 0.0f;
    state.blendElapsed = 0.0f;
    if (from == state.clip) state.previous = nullptr;  // same clip: just restart it
    w.apply(state);
    return true;
}

float AnimationSystem::blendWeight(entt::entity animator) const {
    const auto it = impl_->states.find(animator);
    if (it == impl_->states.end() || it->second.previous == nullptr || it->second.blendDuration <= 0.0f) return 1.0f;
    return std::clamp(it->second.blendElapsed / it->second.blendDuration, 0.0f, 1.0f);
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
