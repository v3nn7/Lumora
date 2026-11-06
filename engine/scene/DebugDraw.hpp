#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <vector>

namespace lumora {

/// Shapes behaviours draw for debugging (rays, trigger volumes, paths). Filled every step like
/// the HUD and drawn on top of the scene (through walls) in the editor while playing, and in
/// the player when debug drawing is on (F3).
struct DebugDraw {
    struct Line {
        glm::vec3 from;
        glm::vec3 to;
        glm::vec4 color;
    };
    struct Box {
        glm::vec3 center;
        glm::vec3 halfExtent;
        glm::vec4 color;
    };
    struct Sphere {
        glm::vec3 center;
        float radius;
        glm::vec4 color;
    };
    std::vector<Line> lines;
    std::vector<Box> boxes;
    std::vector<Sphere> spheres;

    void clear() {
        lines.clear();
        boxes.clear();
        spheres.clear();
    }
    [[nodiscard]] bool empty() const noexcept { return lines.empty() && boxes.empty() && spheres.empty(); }
};

} // namespace lumora
