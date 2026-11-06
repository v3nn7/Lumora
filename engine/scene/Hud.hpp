#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lumora {

/// 2D drawing on top of the game's image (score, health bars, menus), in pixels from the
/// top-left corner of the viewport. Scripts fill it every step; the renderer draws the latest,
/// rectangles and texts in the order they were added (later ones on top).
struct HudCanvas {
    enum class Align : std::int32_t { Left = 0, Center = 1, Right = 2 };
    struct Rect {
        glm::vec2 position;
        glm::vec2 size;
        glm::vec4 color;  // rgb (sRGB) + alpha
        std::uint32_t order = 0;
    };
    struct Text {
        glm::vec2 position;  // top-left of the first line (of the box, when aligned)
        float size;          // pixel height of a line
        glm::vec4 color;
        std::string utf8;
        std::uint32_t order = 0;
        Align align = Align::Left;
        float boxWidth = 0.0f;  // lines are aligned within [x, x + boxWidth]
    };
    std::vector<Rect> rects;
    std::vector<Text> texts;
    glm::vec2 screenSize{1280.0f, 720.0f};  // set by whoever displays the game
    float pixelRatio = 1.0f;                // screen pixels per window coordinate (mouse)

    void addRect(glm::vec2 position, glm::vec2 size, glm::vec4 color) {
        rects.push_back({position, size, color, next_++});
    }
    void addText(glm::vec2 position, float size, glm::vec4 color, std::string utf8, Align align = Align::Left,
                 float boxWidth = 0.0f) {
        texts.push_back({position, size, color, std::move(utf8), next_++, align, boxWidth});
    }
    void clear() {
        rects.clear();
        texts.clear();
        next_ = 0;
    }
    [[nodiscard]] bool empty() const noexcept { return rects.empty() && texts.empty(); }

private:
    std::uint32_t next_ = 0;
};

} // namespace lumora
