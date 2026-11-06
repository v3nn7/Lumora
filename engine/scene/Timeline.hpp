#pragma once

#include "core/Error.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lumora {

/// A cutscene or scripted sequence (.ltimeline): tracks of keys over time, each acting on an
/// entity named by `target` (empty = the entity playing the timeline).
struct TimelineAsset {
    enum class TrackKind { Transform, Visible, Animation, Sound, Event };
    struct Key {
        float time = 0.0f;
        glm::vec3 position{0.0f};     // Transform
        glm::vec3 rotationDeg{0.0f};  // Transform
        bool visible = true;          // Visible
        std::string text;             // Animation: clip; Event: name
    };
    struct Track {
        TrackKind kind = TrackKind::Transform;
        std::string target;
        bool smooth = true;     // Transform: curved path through the keys (else straight lines)
        std::vector<Key> keys;  // sorted by time
    };
    float duration = 5.0f;
    std::vector<Track> tracks;

    /// Inserts (or replaces, at the same time) a key, keeping the track sorted.
    static void setKey(Track& track, const Key& key);
    /// The track for (kind, target), created if missing.
    Track& track(TrackKind kind, const std::string& target);
};

[[nodiscard]] const char* toString(TimelineAsset::TrackKind kind) noexcept;
[[nodiscard]] Result<TimelineAsset> loadTimeline(const std::filesystem::path& file);
Result<> saveTimeline(const TimelineAsset& timeline, const std::filesystem::path& file);

struct TimelinePose {
    glm::vec3 position{0.0f};
    glm::vec3 rotationDeg{0.0f};
};
/// A transform track at `time` (held before the first and after the last key); nullopt if it
/// has no keys.
[[nodiscard]] std::optional<TimelinePose> evaluateTransform(const TimelineAsset::Track& track, float time);
/// A visibility track at `time` (the latest key at or before it).
[[nodiscard]] std::optional<bool> evaluateVisible(const TimelineAsset::Track& track, float time);
/// Keys of a track in (from, to]: what fires when the playhead moves from `from` to `to`.
[[nodiscard]] std::vector<const TimelineAsset::Key*> keysBetween(const TimelineAsset::Track& track, float from, float to);

} // namespace lumora
