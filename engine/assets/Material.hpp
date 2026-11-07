#pragma once

#include "assets/TextureTransform.hpp"
#include "core/Error.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace lumora {

/// A material asset (.lmat, JSON): the surface of a Mesh Renderer, shared by every entity that
/// references it, replacing the renderer's own settings (and those of an imported model). The
/// first fields match the Mesh Renderer's (and their JSON names); the rest match glTF materials
/// and their extensions, so an imported material can be written out without losing anything.
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
    std::string occlusionTexture;  // ambient occlusion (red channel)
    float normalStrength = 1.0f;

    /// Opaque; cut out where the base colour map's alpha is below `alphaCutoff`; or see-through
    /// (blended, `opacity` times the map's alpha).
    enum class AlphaMode : std::int32_t { Opaque = 0, CutOut = 1, Transparent = 2 };
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    float opacity = 1.0f;
    bool doubleSided = false;

    // Texture placement (every map): tiling, offset, rotation in degrees.
    glm::vec2 uvScale{1.0f};
    glm::vec2 uvOffset{0.0f};
    float uvRotation = 0.0f;

    // glTF material extensions (see ImportedMaterial).
    float ior = 1.5f;
    float specularFactor = 1.0f;
    glm::vec3 specularColor{1.0f};
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.0f;
    glm::vec3 sheenColor{0.0f};
    float sheenRoughness = 0.0f;
    float transmission = 0.0f;
    float thickness = 0.0f;
    float attenuationDistance = 0.0f;  // 0 = clear
    glm::vec3 attenuationColor{1.0f};

    [[nodiscard]] TextureTransform uvTransform() const;

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
