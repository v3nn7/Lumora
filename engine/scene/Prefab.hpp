#pragma once

#include "core/Error.hpp"

#include <entt/entity/entity.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>

namespace lumora {

class Scene;

/// Prefabs: an entity with its children saved as a reusable asset (*.lprefab, JSON):
///
///   {"format": "lumora.prefab", "version": 1,
///    "entities": [root, child, ...]}   (SceneSerializer::entityToJson records, root first,
///                                       children after their parent; the root has no parent)
///
/// Instances are independent copies with fresh UUIDs: editing the prefab later does not change
/// entities already placed.
[[nodiscard]] nlohmann::json prefabToJson(const Scene& scene, entt::entity root);
[[nodiscard]] Result<> savePrefab(const Scene& scene, entt::entity root, const std::filesystem::path& file);
[[nodiscard]] Result<nlohmann::json> loadPrefab(const std::filesystem::path& file);
/// Adds a copy of the prefab under `parent` (entt::null = scene root); returns its root.
[[nodiscard]] Result<entt::entity> instantiatePrefab(Scene& scene, const nlohmann::json& prefab,
                                                     entt::entity parent = entt::null);
[[nodiscard]] bool isPrefabFile(const std::filesystem::path& file);

} // namespace lumora
