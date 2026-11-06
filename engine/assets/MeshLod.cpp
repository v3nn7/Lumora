#include "assets/MeshLod.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace lumora {

namespace {

/// Which of the six axis directions a normal is closest to.
std::uint32_t facing(const glm::vec3& n) {
    const glm::vec3 a = glm::abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x >= 0.0f ? 0u : 1u;
    if (a.y >= a.z) return n.y >= 0.0f ? 2u : 3u;
    return n.z >= 0.0f ? 4u : 5u;
}

} // namespace

std::vector<std::uint32_t> simplifyByClustering(const MeshData& mesh, float cellFraction) {
    if (mesh.indices.size() < 3 || mesh.vertices.empty() || !(cellFraction > 0.0f)) return mesh.indices;
    const Aabb b = computeBounds(mesh.vertices);
    const glm::vec3 size = b.max - b.min;
    const float diagonal = glm::length(size);
    if (diagonal <= 0.0f) return mesh.indices;
    const float cell = diagonal * cellFraction;
    const auto cells = glm::max(glm::ivec3(glm::ceil(size / cell)), glm::ivec3(1));

    // Cell (and facing) of every vertex -> the representative: the vertex closest to the
    // cell's average position, so the simplified surface stays on the original one.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> members;
    std::vector<std::uint64_t> keyOf(mesh.vertices.size());
    for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vertex& v = mesh.vertices[i];
        const glm::ivec3 c = glm::clamp(glm::ivec3((v.position - b.min) / cell), glm::ivec3(0), cells - 1);
        const std::uint64_t key = (static_cast<std::uint64_t>(c.x) << 43) ^ (static_cast<std::uint64_t>(c.y) << 22) ^
                                  (static_cast<std::uint64_t>(c.z) << 3) ^ facing(v.normal);
        keyOf[i] = key;
        members[key].push_back(i);
    }
    std::unordered_map<std::uint64_t, std::uint32_t> representative;
    representative.reserve(members.size());
    for (const auto& [key, list] : members) {
        glm::vec3 mean(0.0f);
        for (std::uint32_t i : list) mean += mesh.vertices[i].position;
        mean /= static_cast<float>(list.size());
        std::uint32_t best = list.front();
        float bestDistance = std::numeric_limits<float>::max();
        for (std::uint32_t i : list) {
            const float d = glm::dot(mesh.vertices[i].position - mean, mesh.vertices[i].position - mean);
            if (d < bestDistance) {
                bestDistance = d;
                best = i;
            }
        }
        representative[key] = best;
    }

    std::vector<std::uint32_t> out;
    out.reserve(mesh.indices.size());
    struct TriHash {
        std::size_t operator()(const std::array<std::uint32_t, 3>& t) const noexcept {
            return (static_cast<std::size_t>(t[0]) * 73856093u) ^ (static_cast<std::size_t>(t[1]) * 19349663u) ^
                   (static_cast<std::size_t>(t[2]) * 83492791u);
        }
    };
    std::unordered_set<std::array<std::uint32_t, 3>, TriHash> seen;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        std::array<std::uint32_t, 3> tri{};
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t index = mesh.indices[t + static_cast<std::size_t>(k)];
            if (index >= mesh.vertices.size()) return mesh.indices;  // malformed: keep the original
            tri[static_cast<std::size_t>(k)] = representative[keyOf[index]];
        }
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;  // collapsed
        // Same triangle twice (same winding) is drawn once; rotate so the smallest index leads.
        std::array<std::uint32_t, 3> canonical = tri;
        const auto first = std::ranges::min_element(canonical) - canonical.begin();
        std::ranges::rotate(canonical, canonical.begin() + first);
        if (!seen.insert(canonical).second) continue;
        out.insert(out.end(), tri.begin(), tri.end());
    }
    return out;
}

std::vector<MeshLod> buildLods(const MeshData& mesh, int maxLevels, std::size_t minTriangles) {
    std::vector<MeshLod> levels;
    std::size_t previous = mesh.indices.size() / 3;
    if (previous < minTriangles) return levels;
    // Start fine and coarsen until a level is clearly smaller than the one before.
    float fraction = 1.0f / 128.0f;
    for (int attempt = 0; attempt < 12 && static_cast<int>(levels.size()) < maxLevels; ++attempt, fraction *= 1.6f) {
        std::vector<std::uint32_t> indices = simplifyByClustering(mesh, fraction);
        const std::size_t triangles = indices.size() / 3;
        if (triangles < 8) break;
        if (static_cast<double>(triangles) > static_cast<double>(previous) * 0.6) continue;
        levels.push_back({std::move(indices), fraction});
        previous = triangles;
    }
    return levels;
}

float screenCoverage(float radius, float distance, float fovY) {
    const float tanHalf = std::tan(std::max(fovY, 1e-3f) * 0.5f);
    if (distance <= radius) return 1.0f;
    return std::clamp(radius / (distance * tanHalf), 0.0f, 1.0f);
}

int selectLod(float screenFraction, int levels, float bias) {
    if (levels <= 0 || !(bias > 0.0f)) return 0;
    float threshold = 0.25f / bias;
    int level = 0;
    while (level < levels && screenFraction < threshold) {
        ++level;
        threshold *= 0.5f;
    }
    return level;
}

} // namespace lumora
