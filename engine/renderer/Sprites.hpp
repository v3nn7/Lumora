#pragma once

#include "scene/Camera.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lumora::render {

/// Pictures for sprites, packed into one RGBA8 image (colours as authored, sRGB). Bump
/// `version` whenever the pixels change; the renderer re-uploads then.
struct SpriteAtlas {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::uint64_t version = 0;
};

/// A picture that floats at a point in the world, always facing the viewer and the same size
/// on screen (the editor's light, camera and sound icons). Drawn on top of the final image.
struct Sprite {
    glm::vec3 position{0.0f};
    float size = 32.0f;               // edge length in window pixels (times the batch's pixelRatio)
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};  // atlas rectangle: u0, v0, u1, v1
    glm::vec4 tint{1.0f};             // multiplies the picture (rgb sRGB, alpha)
    std::uint32_t id = 0;             // caller's tag (an entity), returned by layout
};

struct SpriteBatch {
    const SpriteAtlas* atlas = nullptr;
    float pixelRatio = 1.0f;  // target pixels per window pixel
    std::vector<Sprite> sprites;
    void clear() { sprites.clear(); }
    [[nodiscard]] bool empty() const noexcept { return sprites.empty() || atlas == nullptr || atlas->rgba.empty(); }
};

/// Where a sprite lands on a target, in pixels from its top-left corner.
struct SpriteQuad {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
    float distance = 0.0f;  // from the camera
    std::size_t index = 0;  // into SpriteBatch::sprites
};

/// Projects the sprites in front of the camera onto a target of `targetPixels`, farthest first
/// (the drawing order: nearer pictures cover farther ones).
[[nodiscard]] std::vector<SpriteQuad> layoutSprites(const SpriteBatch& batch, const CameraView& camera,
                                                    glm::vec2 targetPixels);

/// The nearest sprite under a target pixel, or nullptr.
[[nodiscard]] const Sprite* spriteAt(const SpriteBatch& batch, const CameraView& camera, glm::vec2 targetPixels,
                                     glm::vec2 pixel);

} // namespace lumora::render
