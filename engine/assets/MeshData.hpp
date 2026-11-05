#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace lumora {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};  // xyz tangent, w = bitangent sign (glTF convention)
};
static_assert(sizeof(Vertex) == 48, "GPU vertex layout");

struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/// CPU-side triangle mesh. Triangles are counter-clockwise when viewed from the front face.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    Aabb bounds;
};

Aabb computeBounds(const std::vector<Vertex>& vertices);
/// Per-vertex tangents from UVs (Lengyel), orthogonalised against the normals. Vertices whose
/// UVs are degenerate get an arbitrary tangent perpendicular to the normal.
void generateTangents(MeshData& mesh);

} // namespace lumora
