#pragma once

#include "core/Error.hpp"
#include "core/Uuid.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumora {

class Scene;

/// Per-entity view of a scene document (SceneSerializer::toJson): every entity record keyed by
/// UUID with its sibling position added as "index", plus the scene-wide settings.
struct SceneIndex {
    std::unordered_map<std::string, nlohmann::json> entities;  // uuid -> record
    std::vector<std::string> order;                            // depth-first: parents first
    nlohmann::json environment;
    std::string name;

    [[nodiscard]] static SceneIndex fromDocument(const nlohmann::json& document);
    [[nodiscard]] static SceneIndex fromScene(const Scene& scene);
};

/// The changes that turn one scene state into another: whole-entity upserts (records as
/// SceneSerializer writes them, plus "index"), removals, and scene-wide settings.
///
/// Undo/redo applies patches instead of reloading whole snapshots, so only the entities an edit
/// touched are restored and concurrent edits by collaborators survive. It is also the
/// collaboration wire format; records from the network are validated like scene files.
struct ScenePatch {
    std::vector<nlohmann::json> upserts;  // parents before children, siblings in order
    std::vector<Uuid> removals;
    std::optional<nlohmann::json> environment;
    std::optional<std::string> name;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] static ScenePatch between(const SceneIndex& from, const SceneIndex& to);
    /// Removals first, then upserts in order. Records whose parent is unknown (e.g. deleted
    /// concurrently) go to the root; invalid records are skipped. Returns the first problem
    /// found, after applying everything that was valid.
    Result<> applyTo(Scene& scene) const;
    /// Every entity the patch creates, changes or removes.
    [[nodiscard]] std::vector<Uuid> touched() const;

    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] static Result<ScenePatch> fromJson(const nlohmann::json& json);
};

} // namespace lumora
