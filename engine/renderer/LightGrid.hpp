#pragma once

#include "renderer/GpuTypes.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <vector>

namespace lumora::render {

/// Screen tiles with the point lights that may reach each one (Forward+ binning on the CPU).
/// `data` starts with (first, count) per tile, row by row; `first` indexes `data` itself, where
/// that tile's light indices follow.
struct LightGrid {
    std::uint32_t tilesX = 0;
    std::uint32_t tilesY = 0;
    std::uint32_t tileSize = kLightTileSize;
    std::vector<std::uint32_t> data;

    [[nodiscard]] std::uint32_t countAt(std::uint32_t x, std::uint32_t y) const { return data[(y * tilesX + x) * 2 + 1]; }
};

/// Bins point lights (positionRange = world position + range) by the screen rectangle their
/// range sphere covers, conservatively (the projected corners of its bounding box). `projection`
/// is Lumora's reversed infinite perspective; lights the camera is inside cover every tile.
[[nodiscard]] LightGrid buildLightGrid(const std::vector<GpuLight>& lights, const glm::mat4& view,
                                       const glm::mat4& projection, std::uint32_t width, std::uint32_t height,
                                       std::uint32_t tileSize = kLightTileSize);

} // namespace lumora::render
