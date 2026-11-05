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
/// UUID, where it sits in the hierarchy, and the scene-wide settings.
struct SceneIndex {
    std::unordered_map<std::string, nlohmann::json> entities;    // uuid -> record as serialized
    std::unordered_map<std::string, std::string> parents;        // uuid -> parent uuid ("" = root)
    std::unordered_map<std::string, std::vector<std::string>> children;  // parent ("" = root) -> ordered
    std::vector<std::string> order;                               // depth-first: parents first
    nlohmann::json environment = nlohmann::json::object();
    std::string name;

    [[nodiscard]] static SceneIndex fromDocument(const nlohmann::json& document);
    [[nodiscard]] static SceneIndex fromScene(const Scene& scene);
    [[nodiscard]] std::size_t positionOf(const std::string& uuid) const;
};

/// The changes that turn one scene state into another, as small as possible:
///
/// - removals;
/// - full records ("full": true) for new entities: the complete entity with its parent and
///   sibling "index";
/// - partial records for changed entities, carrying only what changed: "name", "move"
///   ({"parent", "index"}), changed component fields under "components", ids of components
///   that were added (their fields are all listed) under "added", and of removed ones under
///   "removed";
/// - changed environment settings and the scene name.
///
/// Undo/redo applies these instead of whole snapshots, so only what an edit changed is reverted,
/// and collaboration sends them over the wire: concurrent edits of different fields - even on
/// the same entity - merge, and deleting a sibling never rewrites its neighbours. Edits of
/// entities or components that no longer exist are ignored (a deletion wins over edits).
struct ScenePatch {
    std::vector<nlohmann::json> upserts;  // parents before children, siblings in order
    std::vector<Uuid> removals;
    std::optional<nlohmann::json> environment;  // only the settings that changed
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
