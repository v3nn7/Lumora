#include "renderer/Sprites.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>

namespace lumora::render {

std::vector<SpriteQuad> layoutSprites(const SpriteBatch& batch, const CameraView& camera, glm::vec2 targetPixels) {
    std::vector<SpriteQuad> quads;
    quads.reserve(batch.sprites.size());
    const glm::mat4 viewProj = camera.projection * camera.view;
    for (std::size_t i = 0; i < batch.sprites.size(); ++i) {
        const Sprite& s = batch.sprites[i];
        const glm::vec4 clip = viewProj * glm::vec4(s.position, 1.0f);
        if (clip.w <= camera.nearPlane) continue;  // behind the camera or inside the near plane
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 center((ndc.x * 0.5f + 0.5f) * targetPixels.x, (0.5f - ndc.y * 0.5f) * targetPixels.y);
        const float half = s.size * batch.pixelRatio * 0.5f;
        if (center.x + half < 0.0f || center.y + half < 0.0f || center.x - half > targetPixels.x ||
            center.y - half > targetPixels.y) {
            continue;  // off screen
        }
        // Whole pixels: a picture drawn at its native size stays sharp.
        const glm::vec2 min = glm::round(center - half);
        quads.push_back({min, min + 2.0f * half, glm::distance(camera.position, s.position), i});
    }
    std::ranges::stable_sort(quads, [](const SpriteQuad& a, const SpriteQuad& b) { return a.distance > b.distance; });
    return quads;
}

const Sprite* spriteAt(const SpriteBatch& batch, const CameraView& camera, glm::vec2 targetPixels, glm::vec2 pixel) {
    const std::vector<SpriteQuad> quads = layoutSprites(batch, camera, targetPixels);
    for (auto it = quads.rbegin(); it != quads.rend(); ++it) {  // nearest (drawn last) first
        if (pixel.x >= it->min.x && pixel.x <= it->max.x && pixel.y >= it->min.y && pixel.y <= it->max.y) {
            return &batch.sprites[it->index];
        }
    }
    return nullptr;
}

} // namespace lumora::render
