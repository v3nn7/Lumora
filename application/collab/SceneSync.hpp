#pragma once

#include "scene/ScenePatch.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace lumora {
class Scene;
}

namespace lumora::collab {

/// Keeps a local scene in step with the session.
///
/// Local edits are found by diffing the scene against the state last exchanged with the host
/// (so every editing path - inspector, gizmo, undo, imports - is covered without hooks) and
/// sent as patches. The host orders all patches and echoes them back. Until our own patch comes
/// back, the parts of another participant's changes that ours overrides (the same field, name,
/// placement or component) are skipped: ours is later in the host's order and wins there too,
/// so everyone converges. Changes to different fields - even of one entity - merge.
class SceneSync {
public:
    /// How patches reach the scene (the editor routes them through EditorContext).
    using Applier = std::function<void(const ScenePatch&)>;

    explicit SceneSync(Scene& scene, Applier apply = {});

    /// Treats the current scene as exchanged with the host (after joining or a resync).
    void markSynced();
    /// Replaces the local scene with the host's document; true if anything changed.
    bool replaceWith(const nlohmann::json& document);

    /// Local changes since the last exchange, or nothing. Call before receive().
    [[nodiscard]] std::optional<ScenePatch> takeLocalChanges();
    /// A patch from the host; `own` = the echo of one of our patches.
    void receive(const ScenePatch& patch, bool own);
    /// No local patch is waiting for its echo.
    [[nodiscard]] bool idle() const noexcept { return inFlight_ == 0; }
    [[nodiscard]] Scene& scene() noexcept { return scene_; }

private:
    void apply(const ScenePatch& patch);
    [[nodiscard]] bool pending(const std::string& entity, const std::string& key) const;

    Scene& scene_;
    Applier apply_;
    SceneIndex synced_;
    /// What our unacknowledged patches change: entity uuid ("#scene" for scene-wide settings) ->
    /// change key (see SceneSync.cpp) -> number of patches in flight.
    std::unordered_map<std::string, std::unordered_map<std::string, int>> pending_;
    int inFlight_ = 0;
};

} // namespace lumora::collab
