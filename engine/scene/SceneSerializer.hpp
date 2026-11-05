#pragma once

#include "core/Error.hpp"
#include "scene/Scene.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace lumora {

/// JSON scene format (.lscene). Components are written through ComponentRegistry, so new
/// reflected components serialize without serializer changes.
class SceneSerializer {
public:
    static constexpr int kFormatVersion = 1;

    [[nodiscard]] static nlohmann::json toJson(const Scene& scene);
    [[nodiscard]] static Result<std::unique_ptr<Scene>> fromJson(const nlohmann::json& json);
    /// Replaces the contents of an existing scene in place (UUIDs preserved). Used by undo/redo,
    /// which must keep the Scene object (and every pointer to it) alive. On error the scene is
    /// left cleared.
    [[nodiscard]] static Result<> loadInto(Scene& scene, const nlohmann::json& json);

    [[nodiscard]] static std::string toString(const Scene& scene);
    [[nodiscard]] static Result<std::unique_ptr<Scene>> fromString(const std::string& text);

    [[nodiscard]] static Result<> save(const Scene& scene, const std::filesystem::path& path);
    [[nodiscard]] static Result<std::unique_ptr<Scene>> load(const std::filesystem::path& path);

    /// Deep copy via serialization round trip; UUIDs are preserved.
    [[nodiscard]] static Result<std::unique_ptr<Scene>> clone(const Scene& scene);

    // ---- Building blocks (also used by ScenePatch) -----------------------------------------
    /// {"uuid", "name", "parent"?, "components": {id: {field: value}}} for one entity.
    [[nodiscard]] static nlohmann::json entityToJson(const Scene& scene, entt::entity entity);
    [[nodiscard]] static nlohmann::json environmentToJson(const EnvironmentSettings& environment);
    /// Sets the settings present in `json`; others keep their value.
    static void applyEnvironment(EnvironmentSettings& environment, const nlohmann::json& json);
    /// Adds/updates the components in `components` (validated per field). With `removeMissing`,
    /// removable components not listed are removed, so the entity matches the record exactly.
    static void applyComponents(Scene& scene, entt::entity entity, const nlohmann::json& components,
                                bool removeMissing);
};

} // namespace lumora
