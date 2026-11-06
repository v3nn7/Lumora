#pragma once

#include "assets/MeshData.hpp"

#include <entt/entity/entity.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace lumora {

class Scene;
struct TerrainComponent;
struct FoliageComponent;

/// Heights of a terrain on a square grid centred on its entity (local metres).
struct TerrainData {
    int resolution = 2;
    float size = 1.0f;
    std::vector<float> heights;  // resolution x resolution, rows along +Z

    /// Bilinear height at a local position (clamped to the edges).
    [[nodiscard]] float heightAt(float x, float z) const;
    [[nodiscard]] glm::vec3 normalAt(float x, float z) const;
    [[nodiscard]] glm::vec3 vertex(int ix, int iz) const;
};

/// Changes whenever anything that shapes the terrain changes (fields, heightmap file time).
[[nodiscard]] std::uint64_t terrainKey(const TerrainComponent& terrain, const std::filesystem::path& projectRoot);
/// The terrain's heights: its heightmap (project-relative image, red channel) or generated
/// hills. Shared and cached: the renderer, physics and navigation get the same data.
[[nodiscard]] std::shared_ptr<const TerrainData> terrainData(const TerrainComponent& terrain,
                                                             const std::filesystem::path& projectRoot);
/// Render mesh of a terrain (local space; uv in metres / tiling).
[[nodiscard]] MeshData buildTerrainMesh(const TerrainData& data, float textureTiling);
/// The terrain's triangles in world space (counter-clockwise seen from above).
void terrainTriangles(const TerrainData& data, const glm::mat4& world, std::vector<glm::vec3>& vertices,
                      std::vector<std::uint32_t>& indices);

struct GroundHit {
    float height = 0.0f;
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};
/// Terrain surface below a world point (x, z), if a terrain covers it.
[[nodiscard]] std::optional<GroundHit> terrainGroundAt(const Scene& scene, float x, float z,
                                                       const std::filesystem::path& projectRoot);

/// Changes whenever the scattered copies would (fields, the entity's place, the terrains).
[[nodiscard]] std::uint64_t foliageKey(const Scene& scene, entt::entity entity, const std::filesystem::path& projectRoot);
/// World matrices of a Foliage component's copies (deterministic for its seed).
[[nodiscard]] std::vector<glm::mat4> foliageInstances(const Scene& scene, entt::entity entity,
                                                      const std::filesystem::path& projectRoot);

/// Unit water grid (-0.5..0.5 on X and Z, y = 0) with `cells` x `cells` quads.
[[nodiscard]] MeshData buildWaterGrid(int cells);

} // namespace lumora
