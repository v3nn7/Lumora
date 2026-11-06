#pragma once

#include "renderer/vulkan/Resources.hpp"
#include "scene/Hud.hpp"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace lumora::rhi {
class BindlessTable;
class UploadQueue;
}

namespace lumora::render {

/// One HUD vertex: pixel position, atlas uv (x < 0 = solid), colour.
struct HudVertex {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec4 color;
};
static_assert(sizeof(HudVertex) == 32);

/// The HUD font, rasterised once into an atlas (stb_truetype, 2x oversampled), and the layout
/// of HudCanvas contents into triangles.
class HudFont {
public:
    static constexpr float kBakeSize = 48.0f;  // pixel height the atlas is rasterised at

    HudFont();
    /// Width in pixels of one line of text at `size` pixels.
    [[nodiscard]] float measure(std::string_view utf8, float size) const;
    /// Appends a canvas's rectangles and text as triangles (two per quad).
    void layout(const HudCanvas& canvas, std::vector<HudVertex>& out) const;

    [[nodiscard]] std::uint32_t atlasWidth() const noexcept { return kAtlasWidth; }
    [[nodiscard]] std::uint32_t atlasHeight() const noexcept { return kAtlasHeight; }
    [[nodiscard]] const std::vector<std::uint8_t>& atlas() const noexcept { return atlas_; }

private:
    static constexpr std::uint32_t kAtlasWidth = 1024;
    static constexpr std::uint32_t kAtlasHeight = 1024;
    struct Glyph {
        float x0, y0, x1, y1;      // atlas texels
        float xoff, yoff, xoff2, yoff2;
        float advance;
    };
    [[nodiscard]] const Glyph* glyph(char32_t codepoint) const;
    std::vector<std::uint8_t> atlas_;
    std::vector<Glyph> glyphs_;  // ranges: 0x20-0x7E, 0xA0-0x17F
    float ascent_ = 0.0f;
};

} // namespace lumora::render
