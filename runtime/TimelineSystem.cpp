#include "runtime/TimelineSystem.hpp"

#include "core/Log.hpp"
#include "runtime/AnimationSystem.hpp"
#include "runtime/AudioWorld.hpp"
#include "scene/Scene.hpp"
#include "scene/Timeline.hpp"

#include <algorithm>

namespace lumora {

TimelineSystem::TimelineSystem(Scene& scene, std::filesystem::path projectRoot)
    : scene_(scene), root_(std::move(projectRoot)) {}

TimelineSystem::~TimelineSystem() = default;

bool TimelineSystem::sceneHasTimelines(const Scene& scene) { return !scene.registry().view<TimelineComponent>().empty(); }

bool TimelineSystem::bind(entt::entity entity, State& state) {
    const auto* tc = scene_.registry().try_get<TimelineComponent>(entity);
    if (tc == nullptr || tc->timeline.empty()) return false;
    if (state.asset && state.file == tc->timeline) return true;
    state.file = tc->timeline;
    auto loaded = loadTimeline(root_ / tc->timeline);
    if (!loaded) {
        log::error("Timeline", "{}", loaded.error().message);
        state.asset = std::make_shared<TimelineAsset>();  // empty: reported once
        return false;
    }
    state.asset = std::make_shared<TimelineAsset>(std::move(*loaded));
    return true;
}

bool TimelineSystem::play(entt::entity entity) {
    State& state = states_[entity];
    if (!bind(entity, state)) return false;
    state.time = 0.0f;
    state.playing = true;
    state.started = true;
    apply(entity, state, -1.0f, 0.0f, nullptr, nullptr);  // the first frame's pose now
    return true;
}

void TimelineSystem::stop(entt::entity entity) {
    if (auto it = states_.find(entity); it != states_.end()) it->second.playing = false;
}

bool TimelineSystem::isPlaying(entt::entity entity) const {
    const auto it = states_.find(entity);
    return it != states_.end() && it->second.playing;
}

float TimelineSystem::time(entt::entity entity) const {
    const auto it = states_.find(entity);
    return it != states_.end() ? it->second.time : 0.0f;
}

bool TimelineSystem::eventFired(std::string_view name) const { return std::ranges::find(events_, name) != events_.end(); }

void TimelineSystem::apply(entt::entity owner, State& state, float from, float to, AnimationSystem* animation, AudioWorld* audio) {
    auto& reg = scene_.registry();
    auto targetOf = [&](const std::string& name) -> entt::entity {
        if (name.empty()) return owner;
        for (auto e : reg.view<TransformComponent>()) {
            if (scene_.nameOf(e) == name) return e;
        }
        return entt::null;
    };
    for (const auto& track : state.asset->tracks) {
        const entt::entity target = targetOf(track.target);
        using Kind = TimelineAsset::TrackKind;
        if (track.kind == Kind::Event) {
            for (const auto* key : keysBetween(track, from, to)) events_.push_back(key->text);
            continue;
        }
        if (target == entt::null) continue;
        switch (track.kind) {
            case Kind::Transform:
                if (const auto pose = evaluateTransform(track, to); pose && reg.all_of<TransformComponent>(target)) {
                    auto& t = reg.get<TransformComponent>(target);
                    t.position = pose->position;
                    t.rotationDeg = pose->rotationDeg;
                }
                break;
            case Kind::Visible:
                if (auto* mesh = reg.try_get<MeshRendererComponent>(target)) {
                    if (const auto visible = evaluateVisible(track, to)) mesh->visible = *visible;
                }
                break;
            case Kind::Animation:
                if (animation != nullptr) {
                    for (const auto* key : keysBetween(track, from, to)) animation->play(target, key->text);
                }
                break;
            case Kind::Sound:
                if (audio != nullptr && !keysBetween(track, from, to).empty()) audio->play(target);
                break;
            case Kind::Event: break;
        }
    }
}

void TimelineSystem::update(float dt, AnimationSystem* animation, AudioWorld* audio) {
    events_.clear();
    auto& reg = scene_.registry();
    std::erase_if(states_, [&](const auto& entry) { return !reg.valid(entry.first) || !reg.all_of<TimelineComponent>(entry.first); });
    for (auto [e, tc] : reg.view<TimelineComponent>().each()) {
        State& state = states_[e];
        if (!state.started) {
            state.started = true;
            if (tc.playOnStart && bind(e, state)) {
                state.playing = true;
                state.time = 0.0f;
                apply(e, state, -1.0f, 0.0f, animation, audio);
            }
        }
        if (!state.playing || !bind(e, state)) continue;
        const float duration = std::max(state.asset->duration, 0.01f);
        const float from = state.time;
        float to = from + dt * tc.speed;
        if (to >= duration && tc.loop) {
            apply(e, state, from, duration, animation, audio);  // the keys up to the end, then around
            to = std::fmod(to, duration);
            apply(e, state, -1.0f, to, animation, audio);
        } else {
            if (to >= duration) {
                to = duration;
                state.playing = false;
            }
            apply(e, state, from, to, animation, audio);
        }
        state.time = to;
    }
}

} // namespace lumora
