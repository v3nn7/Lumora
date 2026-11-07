#include "renderer/HudRenderer.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace lumora::render {

extern const unsigned char kHudFont[];
extern const std::size_t kHudFontSize;

struct HudFont::Font {
    std::vector<std::byte> data;
    stbtt_fontinfo info{};
    float scale = 1.0f;   // font units to field pixels
    float ascent = 0.0f;  // field pixels
};

namespace {

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

constexpr unsigned char kOnEdge = 128;  // field value on the outline (0.5 in the shader)

} // namespace

HudFont::HudFont() : atlas_(static_cast<std::size_t>(kAtlasSize) * kAtlasSize, 0) {
    std::vector<std::byte> builtIn(kHudFontSize);
    std::memcpy(builtIn.data(), kHudFont, kHudFontSize);
    addFont({}, std::move(builtIn));
}

HudFont::~HudFont() = default;

int HudFont::addFont(const std::string& key, std::vector<std::byte> data) {
    if (const auto it = fontKeys_.find(key); it != fontKeys_.end()) return it->second;
    auto font = std::make_unique<Font>();
    font->data = std::move(data);
    const auto* bytes = reinterpret_cast<const unsigned char*>(font->data.data());
    const int offset = font->data.size() >= 12 ? stbtt_GetFontOffsetForIndex(bytes, 0) : -1;
    if (offset < 0 || stbtt_InitFont(&font->info, bytes, offset) == 0) {
        fontKeys_[key] = 0;  // not a font: the built-in one (and no second try)
        return 0;
    }
    font->scale = stbtt_ScaleForPixelHeight(&font->info, kFieldSize);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
    font->ascent = static_cast<float>(ascent) * font->scale;
    const int id = static_cast<int>(fonts_.size());
    fonts_.push_back(std::move(font));
    fontKeys_[key] = id;
    return id;
}

int HudFont::findFont(std::string_view key) const {
    const auto it = fontKeys_.find(std::string(key));
    return it != fontKeys_.end() ? it->second : -1;
}

void HudFont::resetAtlas() {
    std::ranges::fill(atlas_, std::uint8_t{0});
    glyphs_.clear();
    penX_ = penY_ = 1;
    rowHeight_ = 0;
    full_ = false;
    dirty_ = true;
}

const HudFont::Glyph& HudFont::glyph(int font, char32_t cp) {
    font = std::clamp(font, 0, static_cast<int>(fonts_.size()) - 1);
    const std::uint64_t key = (static_cast<std::uint64_t>(font) << 32) | cp;
    if (const auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
    Font& f = *fonts_[static_cast<std::size_t>(font)];
    const int index = stbtt_FindGlyphIndex(&f.info, static_cast<int>(cp));
    if (index == 0 && cp != 0xFFFD && cp != '?') {
        // Not in this font: the built-in one has it, or a question mark stands in.
        const Glyph substitute = font != 0 ? glyph(0, cp) : glyph(0, '?');
        return glyphs_[key] = substitute;
    }
    Glyph g;
    g.font = font;
    int advance = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&f.info, index, &advance, &bearing);
    g.advance = static_cast<float>(advance) * f.scale;
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* field = stbtt_GetGlyphSDF(&f.info, f.scale, index, kPadding, kOnEdge, static_cast<float>(kOnEdge) / kPadding, &w,
                                             &h, &xoff, &yoff);
    if (field != nullptr && w > 0 && h > 0) {
        const auto uw = static_cast<std::uint32_t>(w), uh = static_cast<std::uint32_t>(h);
        if (penX_ + uw + 1 > kAtlasSize) {  // next shelf
            penX_ = 1;
            penY_ += rowHeight_ + 1;
            rowHeight_ = 0;
        }
        if (penY_ + uh + 1 > kAtlasSize) {
            full_ = true;  // drawn blank this time; the atlas starts over before the next layout
        } else {
            for (std::uint32_t row = 0; row < uh; ++row) {
                std::memcpy(&atlas_[static_cast<std::size_t>(penY_ + row) * kAtlasSize + penX_], field + static_cast<std::size_t>(row) * uw, uw);
            }
            const float texel = 1.0f / static_cast<float>(kAtlasSize);
            g.uv0 = glm::vec2(static_cast<float>(penX_), static_cast<float>(penY_)) * texel;
            g.uv1 = glm::vec2(static_cast<float>(penX_ + uw), static_cast<float>(penY_ + uh)) * texel;
            g.offset = {static_cast<float>(xoff), static_cast<float>(yoff)};
            g.size = {static_cast<float>(w), static_cast<float>(h)};
            g.blank = false;
            penX_ += uw + 1;
            rowHeight_ = std::max(rowHeight_, uh);
            dirty_ = true;
        }
    }
    if (field != nullptr) stbtt_FreeSDF(field, nullptr);
    return glyphs_[key] = g;
}

float HudFont::kerning(int font, char32_t a, char32_t b) const {
    if (a == 0) return 0.0f;
    const Font& f = *fonts_[static_cast<std::size_t>(font)];
    return static_cast<float>(stbtt_GetCodepointKernAdvance(&f.info, static_cast<int>(a), static_cast<int>(b))) * f.scale;
}

float HudFont::ascent(int font) const { return fonts_[static_cast<std::size_t>(font)]->ascent; }

float HudFont::measure(std::string_view utf8, float size, int font) {
    font = std::clamp(font, 0, static_cast<int>(fonts_.size()) - 1);
    const float scale = size / kFieldSize;
    float width = 0.0f, line = 0.0f;
    char32_t previous = 0;
    int previousFont = -1;
    for (std::size_t i = 0; i < utf8.size();) {
        const char32_t cp = nextCodepoint(utf8, i);
        if (cp == '\n') {
            width = std::max(width, line);
            line = 0.0f;
            previous = 0;
            continue;
        }
        const Glyph& g = glyph(font, cp);
        if (g.font == previousFont) line += kerning(g.font, previous, cp) * scale;
        line += g.advance * scale;
        previous = cp;
        previousFont = g.font;
    }
    return std::max(width, line);
}

void HudFont::layout(const HudCanvas& canvas, std::vector<HudVertex>& out, std::vector<Batch>& batches,
                     const std::function<std::uint32_t(const std::string&)>& image,
                     const std::function<int(const std::string&)>& font) {
    if (full_) resetAtlas();
    auto quad = [&](glm::vec2 p0, glm::vec2 p1, glm::vec2 uv0, glm::vec2 uv1, const glm::vec4& color) {
        const HudVertex a{p0, uv0, color}, b{{p1.x, p0.y}, {uv1.x, uv0.y}, color}, c{p1, uv1, color},
            d{{p0.x, p1.y}, {uv0.x, uv1.y}, color};
        out.insert(out.end(), {a, b, c, a, c, d});
        batches.back().count += 6;
    };
    // A new batch when the texture changes (rectangles fit into any).
    auto use = [&](bool picture, std::uint32_t texture) {
        if (!batches.empty() && batches.back().first + batches.back().count == out.size() &&
            (batches.back().picture == picture && (!picture || batches.back().texture == texture))) {
            return;
        }
        batches.push_back({static_cast<std::uint32_t>(out.size()), 0, picture, texture});
    };
    auto text = [&](const HudCanvas::Text& t) {
        const int id = !t.font.empty() && font ? font(t.font) : 0;
        const float scale = t.size / kFieldSize;
        // Each line starts where its alignment within the box puts it.
        auto lineStart = [&](std::size_t from) {
            if (t.align == HudCanvas::Align::Left) return t.position.x;
            const std::size_t end = t.utf8.find('\n', from);
            const float width =
                measure(std::string_view(t.utf8).substr(from, end == std::string::npos ? end : end - from), t.size, id);
            return t.position.x + (t.boxWidth - width) * (t.align == HudCanvas::Align::Center ? 0.5f : 1.0f);
        };
        use(false, 0);
        glm::vec2 pen(lineStart(0), t.position.y + ascent(std::clamp(id, 0, static_cast<int>(fonts_.size()) - 1)) * scale);
        char32_t previous = 0;
        int previousFont = -1;
        for (std::size_t i = 0; i < t.utf8.size();) {
            const char32_t cp = nextCodepoint(t.utf8, i);
            if (cp == '\n') {
                pen = {lineStart(i), pen.y + t.size * 1.2f};
                previous = 0;
                continue;
            }
            const Glyph g = glyph(id, cp);  // (a copy: the map may grow while the line goes on)
            if (g.font == previousFont) pen.x += kerning(g.font, previous, cp) * scale;
            if (!g.blank) {
                const glm::vec2 p0 = pen + g.offset * scale;
                quad(p0, p0 + g.size * scale, g.uv0, g.uv1, t.color);
            }
            pen.x += g.advance * scale;
            previous = cp;
            previousFont = g.font;
        }
    };
    // Everything in the order it was added.
    enum class Kind { Rect, Text, Image };
    std::vector<std::tuple<std::uint32_t, Kind, std::size_t>> items;
    items.reserve(canvas.rects.size() + canvas.texts.size() + canvas.images.size());
    for (std::size_t i = 0; i < canvas.rects.size(); ++i) items.emplace_back(canvas.rects[i].order, Kind::Rect, i);
    for (std::size_t i = 0; i < canvas.texts.size(); ++i) items.emplace_back(canvas.texts[i].order, Kind::Text, i);
    for (std::size_t i = 0; i < canvas.images.size(); ++i) items.emplace_back(canvas.images[i].order, Kind::Image, i);
    std::ranges::stable_sort(items, {}, [](const auto& item) { return std::get<0>(item); });
    for (const auto& [order, kind, index] : items) {
        switch (kind) {
            case Kind::Rect: {
                const HudCanvas::Rect& rect = canvas.rects[index];
                if (batches.empty() || batches.back().first + batches.back().count != out.size()) use(false, 0);
                quad(rect.position, rect.position + rect.size, {-1.0f, -1.0f}, {-1.0f, -1.0f}, rect.color);
                break;
            }
            case Kind::Text: text(canvas.texts[index]); break;
            case Kind::Image: {
                const HudCanvas::Image& im = canvas.images[index];
                const std::uint32_t texture = image ? image(im.texture) : UINT32_MAX;
                if (texture == UINT32_MAX) break;
                use(true, texture);
                quad(im.position, im.position + im.size, {im.uv.x, im.uv.y}, {im.uv.z, im.uv.w}, im.color);
                break;
            }
        }
    }
    std::erase_if(batches, [](const Batch& b) { return b.count == 0; });
}

} // namespace lumora::render
