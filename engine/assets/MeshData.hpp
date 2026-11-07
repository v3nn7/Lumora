#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace lumora {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};  // xyz tangent, w = bitangent sign (glTF convention)
    glm::vec2 uv1{0.0f};                        // second texture coordinate set (glTF TEXCOORD_1)
};
static_assert(sizeof(Vertex) == 56, "GPU vertex layout");

/// Skinning influences of one vertex: up to four joints (indices into the skin's joint list)
/// with weights that sum to 1.
struct SkinVertex {
    std::uint16_t joints[4] = {0, 0, 0, 0};
    glm::vec4 weights{1.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(SkinVertex) == 24, "GPU skin vertex layout");

struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/// A blend shape (glTF morph target): offsets added to the vertices in proportion to its weight.
struct MorphTarget {
    std::vector<glm::vec3> positions;  // one per vertex
    std::vector<glm::vec3> normals;    // one per vertex, or empty when the shape keeps the normals
    std::vector<glm::vec3> tangents;   // likewise
};

/// CPU-side triangle mesh. Triangles are counter-clockwise when viewed from the front face.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    Aabb bounds;
    std::vector<SkinVertex> skin;     // empty, or one per vertex for a skinned mesh
    std::vector<MorphTarget> morphs;  // blend shapes, each sized like `vertices`
};

Aabb computeBounds(const std::vector<Vertex>& vertices);
/// Per-vertex tangents from UVs (Lengyel), orthogonalised against the normals. Vertices whose
/// UVs are degenerate get an arbitrary tangent perpendicular to the normal.
void generateTangents(MeshData& mesh);
/// The vertices with the morph targets applied at `weights` (missing weights count as 0);
/// normals and tangents renormalised. Returns the bounds of the result.
Aabb applyMorphTargets(const MeshData& mesh, std::span<const float> weights, std::vector<Vertex>& out);

} // namespace lumora
