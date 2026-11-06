#pragma once

#include "core/Error.hpp"
#include "renderer/SceneRenderer.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace lumora {
class AssetLibrary;
class Scene;
}

namespace lumora::render {

class Renderer;

/// A baked light probe volume: a grid of probes, each the SH9 irradiance / pi (cosine-convolved,
/// like EnvironmentMap::irradiance) of what it saw. Probe (x, y, z) sits at the centre of its
/// cell of the volume's box; its 9 coefficients start at sh[((z * ny + y) * nx + x) * 9].
struct ProbeVolumeData {
    glm::uvec3 counts{0u};
    std::vector<glm::vec3> sh;
};

/// Probes along each axis for a box and spacing: at least 1, at most 64 per axis and 8192 in all
/// (the spacing grows until they fit).
[[nodiscard]] glm::uvec3 probeVolumeCounts(const glm::vec3& size, float spacing);

/// Binary .lpv file: "LPV1", the counts (3 x u32), then the coefficients (float RGB).
[[nodiscard]] Result<> saveProbeVolume(const std::filesystem::path& file, const ProbeVolumeData& data);
[[nodiscard]] Result<ProbeVolumeData> loadProbeVolume(const std::filesystem::path& file);

/// Bakes every light probe volume of the scene: six small HDR views from each probe projected
/// onto SH9, written to <project>/Saved/Probes/*.lpv and set as the volume's `baked` file.
/// Each bounce after the first renders with the previous bounce's volumes on, so light reaches
/// further. `progress` gets 0..1 and may return false to cancel. The file the volume had before
/// is removed unless `keepPrevious`. Returns how many volumes were baked.
[[nodiscard]] Result<int> bakeLightProbeVolumes(Renderer& renderer, Scene& scene, AssetLibrary& assets,
                                                RenderSettings settings = {}, bool keepPrevious = false,
                                                const std::function<bool(float)>& progress = {});

} // namespace lumora::render
