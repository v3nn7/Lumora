#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace lumora::render {

/// Unlit, always-on-top triangle geometry drawn after tone mapping (editor gizmos, debug shapes).
struct OverlayVertex {
    glm::vec3 position{0.0f};
    std::uint32_t color = 0xffffffffu;  // RGBA8, little-endian (R in the low byte)
};

static_assert(sizeof(OverlayVertex) == 16, "must match the overlay pipeline vertex layout");

struct OverlayGeometry {
    std::vector<OverlayVertex> vertices;  // triangle list
    void clear() { vertices.clear(); }
    [[nodiscard]] bool empty() const noexcept { return vertices.empty(); }
};

[[nodiscard]] std::uint32_t packColor(const glm::vec4& rgba) noexcept;

/// Builds simple shaded solids into an OverlayGeometry. Faces are tinted by their orientation
/// towards the viewer so shapes read as 3D without lighting.
class OverlayBuilder {
public:
    OverlayBuilder(OverlayGeometry& out, const glm::vec3& viewDirection) : out_(out), view_(viewDirection) {}

    void triangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec4& color);
    void quad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, const glm::vec4& color);
    void cylinder(const glm::vec3& from, const glm::vec3& to, float radius, const glm::vec4& color, int segments = 12);
    void cone(const glm::vec3& base, const glm::vec3& tip, float radius, const glm::vec4& color, int segments = 16);
    void box(const glm::vec3& center, const glm::vec3& halfExtent, const glm::vec4& color);
    /// Box aligned to the given orthonormal axes.
    void orientedBox(const glm::vec3& center, const glm::vec3 axes[3], float halfExtent, const glm::vec4& color);
    void torus(const glm::vec3& center, const glm::vec3& axis, float radius, float thickness, const glm::vec4& color,
               int segments = 64, int sides = 6);
    /// Box spanned by three (not necessarily unit) half-axis vectors.
    void solidBox(const glm::vec3& center, const glm::vec3 halfAxes[3], const glm::vec4& color);
    /// The 12 edges of such a box as thin cylinders.
    void wireBox(const glm::vec3& center, const glm::vec3 halfAxes[3], float thickness, const glm::vec4& color);

private:
    OverlayGeometry& out_;
    glm::vec3 view_;
};

} // namespace lumora::render
