#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace lumora {

/// glTF KHR_texture_transform: uv' = offset + rotate(rotation) * (scale * uv), rotation in
/// radians counter-clockwise in UV space (as the extension's example has it).
struct TextureTransform {
    glm::vec2 offset{0.0f};
    float rotation = 0.0f;
    glm::vec2 scale{1.0f};
    bool operator==(const TextureTransform&) const = default;
    [[nodiscard]] bool identity() const { return *this == TextureTransform{}; }
    /// Rows of the 2x3 matrix: u' = dot(rowU, (u, v, 1)), v' = dot(rowV, (u, v, 1)).
    [[nodiscard]] glm::vec3 rowU() const {
        const float c = std::cos(rotation), s = std::sin(rotation);
        return {c * scale.x, s * scale.y, offset.x};
    }
    [[nodiscard]] glm::vec3 rowV() const {
        const float c = std::cos(rotation), s = std::sin(rotation);
        return {-s * scale.x, c * scale.y, offset.y};
    }
};

} // namespace lumora
