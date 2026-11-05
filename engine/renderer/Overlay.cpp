#include "renderer/Overlay.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace lumora::render {

std::uint32_t packColor(const glm::vec4& rgba) noexcept {
    const glm::vec4 c = glm::clamp(rgba, 0.0f, 1.0f) * 255.0f + 0.5f;
    return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) |
           (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
}

namespace {
/// Any unit vector perpendicular to `n`.
glm::vec3 perpendicular(const glm::vec3& n) {
    const glm::vec3 helper = std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    return glm::normalize(glm::cross(n, helper));
}
} // namespace

void OverlayBuilder::triangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec4& color) {
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    // Faces turned towards the viewer are brighter; back faces stay readable.
    const float facing = len > 1e-12f ? std::abs(glm::dot(n / len, -view_)) : 1.0f;
    const glm::vec4 shaded(glm::vec3(color) * (0.55f + 0.45f * facing), color.a);
    const std::uint32_t packed = packColor(shaded);
    out_.vertices.push_back({a, packed});
    out_.vertices.push_back({b, packed});
    out_.vertices.push_back({c, packed});
}

void OverlayBuilder::quad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
                          const glm::vec4& color) {
    triangle(a, b, c, color);
    triangle(a, c, d, color);
}

void OverlayBuilder::cylinder(const glm::vec3& from, const glm::vec3& to, float radius, const glm::vec4& color,
                              int segments) {
    const glm::vec3 axis = to - from;
    if (glm::dot(axis, axis) < 1e-12f) return;
    const glm::vec3 n = glm::normalize(axis);
    const glm::vec3 u = perpendicular(n);
    const glm::vec3 v = glm::cross(n, u);
    for (int i = 0; i < segments; ++i) {
        const float a0 = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        const float a1 = glm::two_pi<float>() * static_cast<float>(i + 1) / static_cast<float>(segments);
        const glm::vec3 o0 = (u * std::cos(a0) + v * std::sin(a0)) * radius;
        const glm::vec3 o1 = (u * std::cos(a1) + v * std::sin(a1)) * radius;
        quad(from + o0, from + o1, to + o1, to + o0, color);
    }
}

void OverlayBuilder::cone(const glm::vec3& base, const glm::vec3& tip, float radius, const glm::vec4& color,
                          int segments) {
    const glm::vec3 axis = tip - base;
    if (glm::dot(axis, axis) < 1e-12f) return;
    const glm::vec3 n = glm::normalize(axis);
    const glm::vec3 u = perpendicular(n);
    const glm::vec3 v = glm::cross(n, u);
    for (int i = 0; i < segments; ++i) {
        const float a0 = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        const float a1 = glm::two_pi<float>() * static_cast<float>(i + 1) / static_cast<float>(segments);
        const glm::vec3 p0 = base + (u * std::cos(a0) + v * std::sin(a0)) * radius;
        const glm::vec3 p1 = base + (u * std::cos(a1) + v * std::sin(a1)) * radius;
        triangle(p0, p1, tip, color);
        triangle(p1, p0, base, color * glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
    }
}

void OverlayBuilder::orientedBox(const glm::vec3& c, const glm::vec3 axes[3], float h, const glm::vec4& color) {
    const glm::vec3 x = axes[0] * h, y = axes[1] * h, z = axes[2] * h;
    const glm::vec3 p[8] = {c - x - y - z, c + x - y - z, c + x + y - z, c - x + y - z,
                            c - x - y + z, c + x - y + z, c + x + y + z, c - x + y + z};
    quad(p[0], p[1], p[2], p[3], color);
    quad(p[5], p[4], p[7], p[6], color);
    quad(p[4], p[0], p[3], p[7], color);
    quad(p[1], p[5], p[6], p[2], color);
    quad(p[3], p[2], p[6], p[7], color);
    quad(p[4], p[5], p[1], p[0], color);
}

void OverlayBuilder::box(const glm::vec3& center, const glm::vec3& h, const glm::vec4& color) {
    const glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    // Non-uniform extents: scale the unit axes individually.
    const glm::vec3 scaled[3] = {axes[0] * h.x, axes[1] * h.y, axes[2] * h.z};
    const glm::vec3 x = scaled[0], y = scaled[1], z = scaled[2];
    const glm::vec3 c = center;
    const glm::vec3 p[8] = {c - x - y - z, c + x - y - z, c + x + y - z, c - x + y - z,
                            c - x - y + z, c + x - y + z, c + x + y + z, c - x + y + z};
    quad(p[0], p[1], p[2], p[3], color);
    quad(p[5], p[4], p[7], p[6], color);
    quad(p[4], p[0], p[3], p[7], color);
    quad(p[1], p[5], p[6], p[2], color);
    quad(p[3], p[2], p[6], p[7], color);
    quad(p[4], p[5], p[1], p[0], color);
}

void OverlayBuilder::torus(const glm::vec3& center, const glm::vec3& axis, float radius, float thickness,
                           const glm::vec4& color, int segments, int sides) {
    const glm::vec3 n = glm::normalize(axis);
    const glm::vec3 u = perpendicular(n);
    const glm::vec3 v = glm::cross(n, u);
    auto point = [&](int i, int j) {
        const float a = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        const float b = glm::two_pi<float>() * static_cast<float>(j) / static_cast<float>(sides);
        const glm::vec3 radial = u * std::cos(a) + v * std::sin(a);
        return center + radial * (radius + thickness * std::cos(b)) + n * (thickness * std::sin(b));
    };
    for (int i = 0; i < segments; ++i) {
        for (int j = 0; j < sides; ++j) {
            quad(point(i, j), point(i + 1, j), point(i + 1, j + 1), point(i, j + 1), color);
        }
    }
}

} // namespace lumora::render
