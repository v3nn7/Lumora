#pragma once

#include "core/Error.hpp"

#include <glm/vec3.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace lumora {

/// A material asset (.lmat, JSON): the surface of a Mesh Renderer, shared by every entity that
/// references it. Same fields (and JSON names) as the Mesh Renderer's own material settings.
struct MaterialAsset {
    glm::vec3 baseColor{0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive{0.0f};
    float emissiveStrength = 0.0f;
    std::int32_t pattern = 0;  // SurfacePattern
    std::string baseColorTexture;
    std::string metallicRoughnessTexture;
    std::string normalTexture;
    std::string emissiveTexture;

    friend bool operator==(const MaterialAsset&, const MaterialAsset&) = default;
};

inline constexpr const char* kMaterialExtension = ".lmat";

[[nodiscard]] nlohmann::json materialToJson(const MaterialAsset& material);
/// Missing fields keep their defaults; wrong types are errors.
[[nodiscard]] Result<MaterialAsset> materialFromJson(const nlohmann::json& json);
[[nodiscard]] Result<MaterialAsset> loadMaterial(const std::filesystem::path& file);
/// Atomic write (temporary file + rename): the renderer never reads half a file.
[[nodiscard]] Result<> saveMaterial(const MaterialAsset& material, const std::filesystem::path& file);

} // namespace lumora
