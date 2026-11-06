#include "renderer/HudRenderer.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <cmath>

namespace lumora::render {

extern const unsigned char kHudFont[];
extern const std::size_t kHudFontSize;

namespace {
constexpr char32_t kRanges[2][2] = {{0x20, 0x7E}, {0xA0, 0x17F}};

/// Next code point of a UTF-8 string (invalid bytes become U+FFFD).
char32_t nextCodepoint(std::string_view text, std::size_t& i) {
    const auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    char32_t cp = c & (0x3F >> extra);
    for (; extra > 0; --extra) {
        if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 0x3F);
    }
    return cp;
}
} // namespace

HudFont::HudFont() : atlas_(static_cast<std::size_t>(kAtlasWidth) * kAtlasHeight, 0) {
    stbtt_pack_context pack;
    stbtt_PackBegin(&pack, atlas_.data(), static_cast<int>(kAtlasWidth), static_cast<int>(kAtlasHeight), 0, 1, nullptr);
    stbtt_PackSetOversampling(&pack, 2, 2);
    std::vector<stbtt_packedchar> packed;
    for (const auto& range : kRanges) {
        const int count = static_cast<int>(range[1] - range[0] + 1);
        std::vector<stbtt_packedchar> chars(static_cast<std::size_t>(count));
        stbtt_PackFontRange(&pack, kHudFont, 0, kBakeSize, static_cast<int>(range[0]), count, chars.data());
        packed.insert(packed.end(), chars.begin(), chars.end());
    }
    stbtt_PackEnd(&pack);
    for (const stbtt_packedchar& pc : packed) {
        glyphs_.push_back({static_cast<float>(pc.x0), static_cast<float>(pc.y0), static_cast<float>(pc.x1),
                           static_cast<float>(pc.y1), pc.xoff, pc.yoff, pc.xoff2, pc.yoff2, pc.xadvance});
    }
    stbtt_fontinfo info;
    stbtt_InitFont(&info, kHudFont, 0);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    ascent_ = static_cast<float>(ascent) * stbtt_ScaleForPixelHeight(&info, kBakeSize);
}

const HudFont::Glyph* HudFont::glyph(char32_t cp) const {
    std::size_t base = 0;
    for (const auto& range : kRanges) {
        if (cp >= range[0] && cp <= range[1]) return &glyphs_[base + (cp - range[0])];
        base += range[1] - range[0] + 1;
    }
    return &glyphs_['?' - 0x20];
}

float HudFont::measure(std::string_view utf8, float size) const {
    const float scale = size / kBakeSize;
    float width = 0.0f, line = 0.0f;
    for (std::size_t i = 0; i < utf8.size();) {
        const char32_t cp = nextCodepoint(utf8, i);
        if (cp == '\n') {
            width = std::max(width, line);
            line = 0.0f;
            continue;
        }
        line += glyph(cp)->advance * scale;
    }
    return std::max(width, line);
}

void HudFont::layout(const HudCanvas& canvas, std::vector<HudVertex>& out) const {
    auto quad = [&](glm::vec2 p0, glm::vec2 p1, glm::vec2 uv0, glm::vec2 uv1, const glm::vec4& color) {
        const HudVertex a{p0, uv0, color}, b{{p1.x, p0.y}, {uv1.x, uv0.y}, color}, c{p1, uv1, color},
            d{{p0.x, p1.y}, {uv0.x, uv1.y}, color};
        out.insert(out.end(), {a, b, c, a, c, d});
    };
    for (const HudCanvas::Rect& r : canvas.rects) {
        quad(r.position, r.position + r.size, {-1.0f, -1.0f}, {-1.0f, -1.0f}, r.color);
    }
    const glm::vec2 texel(1.0f / static_cast<float>(kAtlasWidth), 1.0f / static_cast<float>(kAtlasHeight));
    for (const HudCanvas::Text& t : canvas.texts) {
        const float scale = t.size / kBakeSize;
        glm::vec2 pen(t.position.x, t.position.y + ascent_ * scale);
        for (std::size_t i = 0; i < t.utf8.size();) {
            const char32_t cp = nextCodepoint(t.utf8, i);
            if (cp == '\n') {
                pen = {t.position.x, pen.y + t.size * 1.2f};
                continue;
            }
            const Glyph& g = *glyph(cp);
            const bool blank = cp == ' ' || cp == 0xA0 || g.x1 <= g.x0 || g.xoff2 - g.xoff < 0.01f;
            if (!blank) {
                quad(pen + glm::vec2(g.xoff, g.yoff) * scale, pen + glm::vec2(g.xoff2, g.yoff2) * scale,
                     glm::vec2(g.x0, g.y0) * texel, glm::vec2(g.x1, g.y1) * texel, t.color);
            }
            pen.x += g.advance * scale;
        }
    }
}

} // namespace lumora::render
