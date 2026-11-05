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

    [[nodiscard]] static std::string toString(const Scene& scene);
    [[nodiscard]] static Result<std::unique_ptr<Scene>> fromString(const std::string& text);

    [[nodiscard]] static Result<> save(const Scene& scene, const std::filesystem::path& path);
    [[nodiscard]] static Result<std::unique_ptr<Scene>> load(const std::filesystem::path& path);

    /// Deep copy via serialization round trip; UUIDs are preserved.
    [[nodiscard]] static Result<std::unique_ptr<Scene>> clone(const Scene& scene);
};

} // namespace lumora
