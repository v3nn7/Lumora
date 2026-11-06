// Levels of detail for meshes: simplified index lists over the mesh's own vertices.
//
// A level is made by vertex clustering: the mesh's bounds are divided into a grid, the vertices
// of one cell (that also face roughly the same way, so hard edges and thin walls survive) merge
// into one representative vertex, and triangles that collapse are dropped. The levels reuse the
// original vertex buffer (and skin weights), so only an index buffer per level is stored.
#pragma once

#include "assets/MeshData.hpp"

#include <cstdint>
#include <vector>

namespace lumora {

struct MeshLod {
    std::vector<std::uint32_t> indices;  // into the original vertices
    float cellFraction = 0.0f;           // grid cell size as a fraction of the bounds' diagonal
};

/// One simplified index list (see above). `cellFraction` > 0; larger = coarser.
[[nodiscard]] std::vector<std::uint32_t> simplifyByClustering(const MeshData& mesh, float cellFraction);

/// Up to `maxLevels` coarser levels (LOD1, LOD2, ...), each with at most ~60 % of the previous
/// level's triangles; none for meshes under `minTriangles`.
[[nodiscard]] std::vector<MeshLod> buildLods(const MeshData& mesh, int maxLevels = 3, std::size_t minTriangles = 256);

/// Which level to draw (0 = full detail) for an object whose bounding sphere covers
/// `screenFraction` of the screen height. Each level halves the coverage at which it starts:
/// LOD1 below 25 %, LOD2 below 12 %, LOD3 below 6 % (bias > 1 keeps detail longer, 0 = always 0).
[[nodiscard]] int selectLod(float screenFraction, int levels, float bias = 1.0f);

/// Fraction of the screen height covered by a sphere seen from `distance` with a vertical
/// field of view of `fovY` radians.
[[nodiscard]] float screenCoverage(float radius, float distance, float fovY);

} // namespace lumora
