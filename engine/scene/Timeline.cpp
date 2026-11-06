#include "scene/Timeline.hpp"

#include <glm/gtx/euler_angles.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace lumora {

namespace {

constexpr const char* kKindNames[] = {"transform", "visible", "animation", "sound", "event"};

nlohmann::json vec(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }

glm::vec3 vec(const nlohmann::json& j, glm::vec3 fallback) {
    if (!j.is_array() || j.size() != 3) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

glm::quat toQuat(const glm::vec3& deg) {
    const glm::vec3 r = glm::radians(deg);
    return glm::quat_cast(glm::eulerAngleYXZ(r.y, r.x, r.z));
}

glm::vec3 toEulerDeg(const glm::quat& q) {
    float y = 0.0f, x = 0.0f, z = 0.0f;
    glm::extractEulerAngleYXZ(glm::mat4_cast(glm::normalize(q)), y, x, z);
    return glm::degrees(glm::vec3(x, y, z));
}

glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

} // namespace

const char* toString(TimelineAsset::TrackKind kind) noexcept { return kKindNames[static_cast<int>(kind)]; }

void TimelineAsset::setKey(Track& track, const Key& key) {
    auto it = std::ranges::lower_bound(track.keys, key.time, {}, &Key::time);
    if (it != track.keys.end() && std::abs(it->time - key.time) < 1e-4f) {
        *it = key;
    } else {
        track.keys.insert(it, key);
    }
}

TimelineAsset::Track& TimelineAsset::track(TrackKind kind, const std::string& target) {
    for (Track& t : tracks) {
        if (t.kind == kind && t.target == target) return t;
    }
    Track t;
    t.kind = kind;
    t.target = target;
    tracks.push_back(std::move(t));
    return tracks.back();
}

Result<TimelineAsset> loadTimeline(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return makeError("cannot open {}", file.string());
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || j.value("format", "") != "lumora.timeline") return makeError("{} is not a Lumora timeline", file.string());
    TimelineAsset t;
    t.duration = std::max(j.value("duration", t.duration), 0.01f);
    for (const auto& jt : j.value("tracks", nlohmann::json::array())) {
        TimelineAsset::Track track;
        const std::string kind = jt.value("kind", "transform");
        const auto found = std::ranges::find_if(kKindNames, [&](const char* n) { return kind == n; });
        if (found == std::end(kKindNames)) continue;  // from a newer version: skipped
        track.kind = static_cast<TimelineAsset::TrackKind>(found - std::begin(kKindNames));
        track.target = jt.value("target", "");
        track.smooth = jt.value("smooth", true);
        for (const auto& jk : jt.value("keys", nlohmann::json::array())) {
            TimelineAsset::Key key;
            key.time = jk.value("t", 0.0f);
            key.position = vec(jk.value("position", nlohmann::json()), key.position);
            key.rotationDeg = vec(jk.value("rotation", nlohmann::json()), key.rotationDeg);
            key.visible = jk.value("visible", true);
            key.text = jk.value("text", "");
            TimelineAsset::setKey(track, key);
        }
        t.tracks.push_back(std::move(track));
    }
    return t;
}

Result<> saveTimeline(const TimelineAsset& t, const std::filesystem::path& file) {
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& track : t.tracks) {
        nlohmann::json keys = nlohmann::json::array();
        for (const auto& k : track.keys) {
            nlohmann::json jk = {{"t", k.time}};
            switch (track.kind) {
                case TimelineAsset::TrackKind::Transform:
                    jk["position"] = vec(k.position);
                    jk["rotation"] = vec(k.rotationDeg);
                    break;
                case TimelineAsset::TrackKind::Visible: jk["visible"] = k.visible; break;
                case TimelineAsset::TrackKind::Animation:
                case TimelineAsset::TrackKind::Event: jk["text"] = k.text; break;
                case TimelineAsset::TrackKind::Sound: break;
            }
            keys.push_back(std::move(jk));
        }
        nlohmann::json jt = {{"kind", toString(track.kind)}, {"target", track.target}, {"keys", std::move(keys)}};
        if (track.kind == TimelineAsset::TrackKind::Transform) jt["smooth"] = track.smooth;
        tracks.push_back(std::move(jt));
    }
    const nlohmann::json j = {{"format", "lumora.timeline"}, {"version", 1}, {"duration", t.duration}, {"tracks", std::move(tracks)}};
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!(out << j.dump(2) << '\n')) return makeError("could not write {}", file.string());
    return {};
}

std::optional<TimelinePose> evaluateTransform(const TimelineAsset::Track& track, float time) {
    const auto& keys = track.keys;
    if (keys.empty()) return std::nullopt;
    if (time <= keys.front().time) return TimelinePose{keys.front().position, keys.front().rotationDeg};
    if (time >= keys.back().time) return TimelinePose{keys.back().position, keys.back().rotationDeg};
    const auto next = std::ranges::upper_bound(keys, time, {}, &TimelineAsset::Key::time);
    const std::size_t i = static_cast<std::size_t>(next - keys.begin()) - 1;
    const auto& a = keys[i];
    const auto& b = keys[i + 1];
    const float span = std::max(b.time - a.time, 1e-6f);
    float u = std::clamp((time - a.time) / span, 0.0f, 1.0f);
    TimelinePose pose;
    if (track.smooth) {
        const glm::vec3& before = i > 0 ? keys[i - 1].position : a.position;
        const glm::vec3& after = i + 2 < keys.size() ? keys[i + 2].position : b.position;
        pose.position = catmullRom(before, a.position, b.position, after, u);
        u = u * u * (3.0f - 2.0f * u);  // ease in and out of each rotation key
    } else {
        pose.position = glm::mix(a.position, b.position, u);
    }
    pose.rotationDeg = toEulerDeg(glm::slerp(toQuat(a.rotationDeg), toQuat(b.rotationDeg), u));
    return pose;
}

std::optional<bool> evaluateVisible(const TimelineAsset::Track& track, float time) {
    std::optional<bool> value;
    for (const auto& k : track.keys) {
        if (k.time > time) break;
        value = k.visible;
    }
    return value;
}

std::vector<const TimelineAsset::Key*> keysBetween(const TimelineAsset::Track& track, float from, float to) {
    std::vector<const TimelineAsset::Key*> out;
    for (const auto& k : track.keys) {
        if (k.time > from && k.time <= to) out.push_back(&k);
    }
    return out;
}

} // namespace lumora
