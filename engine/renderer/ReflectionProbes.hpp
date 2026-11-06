#pragma once

#include "assets/Image.hpp"
#include "core/Error.hpp"
#include "renderer/SceneRenderer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace lumora {
class AssetLibrary;
class Scene;
}

namespace lumora::render {

class Renderer;

/// Cube faces in the order +X, -X, +Y, -Y, +Z, -Z: the direction each looks along and the
/// direction that is up in its image.
struct CubeFace {
    glm::vec3 forward;
    glm::vec3 up;
};
[[nodiscard]] const std::array<CubeFace, 6>& cubeFaces();

/// Equirectangular image (width x width / 2) sampled bilinearly from six square cube faces
/// rendered with a 90 degree field of view along cubeFaces().
[[nodiscard]] HdrImage cubeToEquirect(const std::array<HdrImage, 6>& faces, std::uint32_t width);

/// Bakes every reflection probe of the scene: six HDR views from its position (without
/// reflection probes, bloom or the editor's overlays), written as an equirectangular .hdr under
/// <project>/Saved/Probes/ and set as the probe's `baked` file. The previous file is removed
/// unless `keepPrevious` (the editor keeps it, so undoing the bake still finds it).
/// Returns how many probes were baked.
[[nodiscard]] Result<int> bakeReflectionProbes(Renderer& renderer, Scene& scene, AssetLibrary& assets,
                                               RenderSettings settings = {}, bool keepPrevious = false);

} // namespace lumora::render
