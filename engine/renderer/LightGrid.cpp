#include "renderer/LightGrid.hpp"

#include <glm/common.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lumora::render {

LightGrid buildLightGrid(const std::vector<GpuLight>& lights, const glm::mat4& view, const glm::mat4& projection,
                         std::uint32_t width, std::uint32_t height, std::uint32_t tileSize) {
    LightGrid grid;
    grid.tileSize = std::max(tileSize, 1u);
    grid.tilesX = std::max((width + grid.tileSize - 1) / grid.tileSize, 1u);
    grid.tilesY = std::max((height + grid.tileSize - 1) / grid.tileSize, 1u);
    const std::uint32_t tiles = grid.tilesX * grid.tilesY;
    const float nearPlane = projection[3][2];
    struct Rect {
        std::uint32_t x0, y0, x1, y1;  // inclusive tile range
    };
    std::vector<Rect> rects(lights.size(), Rect{1, 1, 0, 0});  // empty by default
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const glm::vec3 c(view * glm::vec4(glm::vec3(lights[i].positionRange), 1.0f));
        const float r = lights[i].positionRange.w;
        if (r <= 0.0f || c.z - r > 0.0f) continue;  // entirely behind the camera
        if (c.z + r > -nearPlane) {                  // reaches the near plane: assume everywhere
            rects[i] = {0, 0, grid.tilesX - 1, grid.tilesY - 1};
            continue;
        }
        float minX = std::numeric_limits<float>::max(), minY = minX;
        float maxX = -minX, maxY = -minX;
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 p = c + glm::vec3(k & 1 ? r : -r, k & 2 ? r : -r, k & 4 ? r : -r);
            const float d = -p.z;  // > near for every corner here
            const float x = (projection[0][0] * p.x / d * 0.5f + 0.5f) * static_cast<float>(width);
            const float y = (projection[1][1] * p.y / d * 0.5f + 0.5f) * static_cast<float>(height);
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
        if (maxX < 0.0f || maxY < 0.0f || minX >= static_cast<float>(width) || minY >= static_cast<float>(height)) continue;
        auto tile = [&](float v, std::uint32_t count) {
            return static_cast<std::uint32_t>(std::clamp(v / static_cast<float>(grid.tileSize), 0.0f, static_cast<float>(count - 1)));
        };
        rects[i] = {tile(minX, grid.tilesX), tile(minY, grid.tilesY), tile(maxX, grid.tilesX), tile(maxY, grid.tilesY)};
    }
    // Count, then lay the lists out after the (first, count) headers.
    std::vector<std::uint32_t> counts(tiles, 0);
    for (const Rect& rc : rects) {
        for (std::uint32_t y = rc.y0; y <= rc.y1 && rc.x0 <= rc.x1; ++y) {
            for (std::uint32_t x = rc.x0; x <= rc.x1; ++x) ++counts[y * grid.tilesX + x];
        }
    }
    grid.data.assign(tiles * 2, 0);
    std::uint32_t next = tiles * 2;
    for (std::uint32_t t = 0; t < tiles; ++t) {
        grid.data[t * 2] = next;
        next += counts[t];
    }
    grid.data.resize(next);
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const Rect& rc = rects[i];
        for (std::uint32_t y = rc.y0; y <= rc.y1 && rc.x0 <= rc.x1; ++y) {
            for (std::uint32_t x = rc.x0; x <= rc.x1; ++x) {
                const std::uint32_t t = y * grid.tilesX + x;
                grid.data[grid.data[t * 2] + grid.data[t * 2 + 1]++] = static_cast<std::uint32_t>(i);
            }
        }
    }
    return grid;
}

} // namespace lumora::render
