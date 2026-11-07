#pragma once

#include "renderer/vulkan/Resources.hpp"
#include "scene/Hud.hpp"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lumora::render {

/// One HUD vertex: pixel position, uv (x < 0 = solid), colour.
struct HudVertex {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec4 color;
};
static_assert(sizeof(HudVertex) == 32);

/// Text for the HUD: signed distance fields of the glyphs (stb_truetype), sharp at any size,
/// made on first use into one atlas, so any script the fonts cover works (the built-in DejaVu
/// Sans subset: Latin, Greek, Cyrillic, punctuation and symbols) and fonts of the project too.
/// A glyph a font lacks comes from the built-in font, else "?". Also lays out HudCanvas
/// contents as triangles in batches (one per texture run).
class HudFont {
public:
    static constexpr float kFieldSize = 40.0f;          // em height the distance fields are made at
    static constexpr int kPadding = 6;                  // texels of field around each glyph
    static constexpr std::uint32_t kAtlasSize = 2048;   // R8 atlas, square

    HudFont();
    ~HudFont();
    HudFont(const HudFont&) = delete;
    HudFont& operator=(const HudFont&) = delete;

    /// Adds a TrueType / OpenType font under `key`; its id, or 0 (the built-in font) if the
    /// bytes are not a font. The same key returns the same id.
    int addFont(const std::string& key, std::vector<std::byte> data);
    /// Id of a font added under `key`, -1 if none was.
    [[nodiscard]] int findFont(std::string_view key) const;

    /// Width in pixels of the widest line of text at `size` pixels.
    [[nodiscard]] float measure(std::string_view utf8, float size, int font = 0);

    struct Batch {
        std::uint32_t first = 0;   // vertex
        std::uint32_t count = 0;
        bool picture = false;      // an image (texture) instead of the glyph atlas
        std::uint32_t texture = 0; // pictures: the bindless index
    };
    /// Appends a canvas's rectangles, pictures and text as triangles (two per quad), in order,
    /// split into batches. `image` gives a texture key's bindless index (UINT32_MAX: skip the
    /// picture); `font` a font key's id (fonts unknown to it use the built-in one).
    void layout(const HudCanvas& canvas, std::vector<HudVertex>& out, std::vector<Batch>& batches,
                const std::function<std::uint32_t(const std::string&)>& image = {},
                const std::function<int(const std::string&)>& font = {});

    [[nodiscard]] std::uint32_t atlasWidth() const noexcept { return kAtlasSize; }
    [[nodiscard]] std::uint32_t atlasHeight() const noexcept { return kAtlasSize; }
    [[nodiscard]] const std::vector<std::uint8_t>& atlas() const noexcept { return atlas_; }
    /// The atlas gained glyphs since the last call (upload it again).
    bool takeDirty() noexcept {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }
    [[nodiscard]] std::size_t glyphCount() const noexcept { return glyphs_.size(); }

private:
    struct Font;
    struct Glyph {
        glm::vec2 uv0{0.0f}, uv1{0.0f};  // atlas
        glm::vec2 offset{0.0f};          // top-left from the pen, in field pixels (kFieldSize em)
        glm::vec2 size{0.0f};            // quad size in field pixels
        float advance = 0.0f;            // field pixels
        bool blank = true;
        int font = 0;                    // the font that drew it (kerning)
    };
    const Glyph& glyph(int font, char32_t codepoint);
    [[nodiscard]] float kerning(int font, char32_t a, char32_t b) const;
    [[nodiscard]] float ascent(int font) const;
    void resetAtlas();

    std::vector<std::unique_ptr<Font>> fonts_;
    std::unordered_map<std::string, int> fontKeys_;
    std::unordered_map<std::uint64_t, Glyph> glyphs_;
    std::vector<std::uint8_t> atlas_;
    std::uint32_t penX_ = 1, penY_ = 1, rowHeight_ = 0;  // shelf packing
    bool dirty_ = true;
    bool full_ = false;  // the atlas filled up during a layout: it starts over next time
};

} // namespace lumora::render
