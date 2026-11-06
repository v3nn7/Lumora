#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <string>
#include <vector>

namespace lumora {

/// 2D drawing on top of the game's image (score, health bars, messages), in pixels from the
/// top-left corner of the viewport. Scripts fill it every step; the renderer draws the latest.
struct HudCanvas {
    struct Rect {
        glm::vec2 position;
        glm::vec2 size;
        glm::vec4 color;  // rgb (sRGB) + alpha
    };
    struct Text {
        glm::vec2 position;  // top-left of the first line
        float size;          // pixel height of a line
        glm::vec4 color;
        std::string utf8;
    };
    std::vector<Rect> rects;
    std::vector<Text> texts;
    glm::vec2 screenSize{1280.0f, 720.0f};  // set by whoever displays the game

    void clear() {
        rects.clear();
        texts.clear();
    }
    [[nodiscard]] bool empty() const noexcept { return rects.empty() && texts.empty(); }
};

} // namespace lumora
