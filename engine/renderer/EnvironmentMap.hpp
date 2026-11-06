#pragma once

#include "assets/Image.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace lumora::render {

/// Image-based lighting data derived from an equirectangular HDR environment, computed on the
/// CPU (in parallel on the job system) and uploaded once:
///
/// - `specular`: the environment prefiltered with the GGX lobe, one mip per roughness step
///   (mip i holds roughness i / (levels - 1)), sampled along the reflection's dominant
///   direction - the split-sum approximation's first term.
/// - `irradiance`: 3rd-order spherical harmonics of the cosine-convolved environment,
///   pre-divided by pi, so shIrradiance(n) * albedo is the diffuse reflection.
///
/// Directions follow the shader convention: +Y up, u = 0.5 + atan2(x, -z) / 2pi, v = acos(y) / pi.
struct EnvironmentMap {
    struct Level {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<float> rgb;  // linear radiance, rows top (v = 0, +Y) to bottom
    };
    std::vector<Level> specular;             // specular[0] is the sharpest
    std::array<glm::vec3, 9> irradiance{};   // SH9 coefficients (RGB), already convolved and / pi
};

/// Number of roughness levels in EnvironmentMap::specular.
inline constexpr std::uint32_t kEnvironmentSpecularLevels = 6;

/// Prefilters an environment. The specular chain starts at min(512, image width) pixels wide.
[[nodiscard]] EnvironmentMap prefilterEnvironment(const HdrImage& image);

/// SH9 irradiance / pi (cosine-convolved) of an equirectangular radiance image.
[[nodiscard]] std::array<glm::vec3, 9> projectIrradiance(const HdrImage& image);

/// Irradiance / pi from SH9 coefficients (the same formula the shader uses).
[[nodiscard]] glm::vec3 evaluateIrradiance(const std::array<glm::vec3, 9>& sh, const glm::vec3& normal);

/// Unit direction at equirectangular coordinates (u, v) in [0, 1].
[[nodiscard]] glm::vec3 equirectDirection(float u, float v);

} // namespace lumora::render
