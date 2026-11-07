#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lumora {

/// 2D drawing on top of the game's image (score, health bars, menus), in pixels from the
/// top-left corner of the viewport. Scripts fill it every step; the renderer draws the latest:
/// rectangles, pictures and texts in the order they were added (later ones on top).
struct HudCanvas {
    enum class Align : std::int32_t { Left = 0, Center = 1, Right = 2 };
    /// No clipping: x0, y0, x1, y1 of the area items are cut to.
    static constexpr glm::vec4 kNoClip{-1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f};
    struct Rect {
        glm::vec2 position;
        glm::vec2 size;
        glm::vec4 color;  // rgb (sRGB) + alpha
        std::uint32_t order = 0;
        glm::vec4 clip = kNoClip;
    };
    struct Text {
        glm::vec2 position;  // top-left of the first line (of the box, when aligned)
        float size;          // pixel height of a line
        glm::vec4 color;
        std::string utf8;
        std::uint32_t order = 0;
        Align align = Align::Left;
        float boxWidth = 0.0f;  // lines are aligned within [x, x + boxWidth]
        std::string font;       // a font file of the project (.ttf / .otf key); empty = the built-in one
        glm::vec4 clip = kNoClip;
    };
    /// A picture: a texture of the project (image file or "model#texN" key), tinted by `color`.
    struct Image {
        glm::vec2 position;
        glm::vec2 size;
        std::string texture;
        glm::vec4 color{1.0f};
        glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};  // the part of the texture: u0, v0, u1, v1
        std::uint32_t order = 0;
        glm::vec4 clip = kNoClip;
    };
    std::vector<Rect> rects;
    std::vector<Text> texts;
    std::vector<Image> images;
    glm::vec2 screenSize{1280.0f, 720.0f};  // set by whoever displays the game
    float pixelRatio = 1.0f;                // screen pixels per window coordinate (mouse)

    void addRect(glm::vec2 position, glm::vec2 size, glm::vec4 color) {
        rects.push_back({position, size, color, next_++, clip_});
    }
    void addText(glm::vec2 position, float size, glm::vec4 color, std::string utf8, Align align = Align::Left,
                 float boxWidth = 0.0f, std::string font = {}) {
        texts.push_back({position, size, color, std::move(utf8), next_++, align, boxWidth, std::move(font), clip_});
    }
    void addImage(glm::vec2 position, glm::vec2 size, std::string texture, glm::vec4 color = glm::vec4(1.0f),
                  glm::vec4 uv = {0.0f, 0.0f, 1.0f, 1.0f}) {
        images.push_back({position, size, std::move(texture), color, uv, next_++, clip_});
    }
    /// Items added from now on are cut to this area (x0, y0, x1, y1): scrolled lists.
    void setClip(glm::vec4 area) { clip_ = area; }
    void clearClip() { clip_ = kNoClip; }
    [[nodiscard]] glm::vec4 clip() const noexcept { return clip_; }
    void clear() {
        rects.clear();
        texts.clear();
        images.clear();
        next_ = 0;
        clip_ = kNoClip;
    }
    [[nodiscard]] bool empty() const noexcept { return rects.empty() && texts.empty() && images.empty(); }

private:
    std::uint32_t next_ = 0;
    glm::vec4 clip_ = kNoClip;
};

} // namespace lumora
