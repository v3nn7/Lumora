#include "scene/Landscape.hpp"

#include "assets/Image.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/constants.hpp>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lumora {

namespace {

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}
std::uint64_t mixFloat(std::uint64_t h, float f) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(f));
    std::memcpy(&bits, &f, sizeof(bits));
    return mix(h, bits);
}
std::uint64_t mixString(std::uint64_t h, const std::string& s) {
    return mix(h, std::hash<std::string>{}(s));
}
std::uint64_t mixMatrix(std::uint64_t h, const glm::mat4& m) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) h = mixFloat(h, m[c][r]);
    }
    return h;
}

/// Lattice value in [0, 1) for integer coordinates.
float lattice(int x, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(z) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

float valueNoise(float x, float z, std::uint32_t seed) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);  // quintic fade
    tz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);
    const float a = lattice(ix, iz, seed), b = lattice(ix + 1, iz, seed);
    const float c = lattice(ix, iz + 1, seed), d = lattice(ix + 1, iz + 1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

/// Fractal hills in [0, 1].
float hills(float x, float z, std::uint32_t seed) {
    float sum = 0.0f, amplitude = 1.0f, total = 0.0f, frequency = 1.0f;
    for (int octave = 0; octave < 5; ++octave) {
        sum += valueNoise(x * frequency, z * frequency, seed + static_cast<std::uint32_t>(octave) * 101u) * amplitude;
        total += amplitude;
        amplitude *= 0.45f;
        frequency *= 2.03f;
    }
    return sum / total;
}

struct SplitMix {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float uniform() { return static_cast<float>(next() >> 40) / static_cast<float>(1ull << 24); }
};

} // namespace

float TerrainData::heightAt(float x, float z) const {
    const float cell = size / static_cast<float>(resolution - 1);
    const float gx = std::clamp((x + size * 0.5f) / cell, 0.0f, static_cast<float>(resolution - 1));
    const float gz = std::clamp((z + size * 0.5f) / cell, 0.0f, static_cast<float>(resolution - 1));
    const int x0 = std::min(static_cast<int>(gx), resolution - 2), z0 = std::min(static_cast<int>(gz), resolution - 2);
    const float tx = gx - static_cast<float>(x0), tz = gz - static_cast<float>(z0);
    auto h = [&](int ix, int iz) { return heights[static_cast<std::size_t>(iz) * static_cast<std::size_t>(resolution) + static_cast<std::size_t>(ix)]; };
    const float top = h(x0, z0) + (h(x0 + 1, z0) - h(x0, z0)) * tx;
    const float bottom = h(x0, z0 + 1) + (h(x0 + 1, z0 + 1) - h(x0, z0 + 1)) * tx;
    return top + (bottom - top) * tz;
}

glm::vec3 TerrainData::normalAt(float x, float z) const {
    const float e = size / static_cast<float>(resolution - 1);
    const float dx = heightAt(x + e, z) - heightAt(x - e, z);
    const float dz = heightAt(x, z + e) - heightAt(x, z - e);
    return glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
}

glm::vec3 TerrainData::vertex(int ix, int iz) const {
    const float cell = size / static_cast<float>(resolution - 1);
    return {-size * 0.5f + cell * static_cast<float>(ix),
            heights[static_cast<std::size_t>(iz) * static_cast<std::size_t>(resolution) + static_cast<std::size_t>(ix)],
            -size * 0.5f + cell * static_cast<float>(iz)};
}

std::uint64_t terrainKey(const TerrainComponent& t, const std::filesystem::path& projectRoot) {
    std::uint64_t h = 0x7E77A1Full;
    h = mixFloat(h, t.size);
    h = mixFloat(h, t.height);
    h = mix(h, static_cast<std::uint64_t>(t.resolution));
    h = mix(h, static_cast<std::uint64_t>(t.seed));
    h = mixFloat(h, t.hillSize);
    h = mixString(h, t.heightmap);
    if (!t.heightmap.empty()) {
        h = mixString(h, projectRoot.generic_string());
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(projectRoot / t.heightmap, ec);
        if (!ec) h = mix(h, static_cast<std::uint64_t>(time.time_since_epoch().count()));
    }
    return h;
}

std::shared_ptr<const TerrainData> terrainData(const TerrainComponent& t, const std::filesystem::path& projectRoot) {
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, std::shared_ptr<const TerrainData>> cache;
    const std::uint64_t key = terrainKey(t, projectRoot);
    {
        std::scoped_lock lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    auto data = std::make_shared<TerrainData>();
    data->resolution = std::clamp(t.resolution, 2, 1025);
    data->size = std::max(t.size, 0.01f);
    const int n = data->resolution;
    data->heights.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0.0f);
    std::optional<ImageData> image;
    if (!t.heightmap.empty() && !projectRoot.empty()) {  // (without a project: hills until it is known)
        auto loaded = loadImageFile(projectRoot / t.heightmap);
        if (loaded && loaded->width > 0 && loaded->height > 0) {
            image = std::move(*loaded);
        } else {
            log::warn("Terrain", "could not read heightmap '{}': using generated hills", t.heightmap);
        }
    }
    for (int iz = 0; iz < n; ++iz) {
        for (int ix = 0; ix < n; ++ix) {
            const float u = static_cast<float>(ix) / static_cast<float>(n - 1);
            const float v = static_cast<float>(iz) / static_cast<float>(n - 1);
            float h = 0.0f;
            if (image) {
                // Bilinear over the red channel (image rows go along +Z).
                const float px = u * static_cast<float>(image->width - 1), py = v * static_cast<float>(image->height - 1);
                const auto x0 = static_cast<std::uint32_t>(px), y0 = static_cast<std::uint32_t>(py);
                const std::uint32_t x1 = std::min(x0 + 1, image->width - 1), y1 = std::min(y0 + 1, image->height - 1);
                auto r = [&](std::uint32_t x, std::uint32_t y) {
                    return static_cast<float>(image->rgba[(static_cast<std::size_t>(y) * image->width + x) * 4]) / 255.0f;
                };
                const float tx = px - static_cast<float>(x0), ty = py - static_cast<float>(y0);
                const float top = r(x0, y0) + (r(x1, y0) - r(x0, y0)) * tx;
                const float bottom = r(x0, y1) + (r(x1, y1) - r(x0, y1)) * tx;
                h = top + (bottom - top) * ty;
            } else {
                const float scale = 1.0f / std::max(t.hillSize, 0.01f);
                const float x = (u - 0.5f) * data->size * scale, z = (v - 0.5f) * data->size * scale;
                h = hills(x, z, static_cast<std::uint32_t>(t.seed));
            }
            data->heights[static_cast<std::size_t>(iz) * static_cast<std::size_t>(n) + static_cast<std::size_t>(ix)] = h * t.height;
        }
    }
    std::scoped_lock lock(mutex);
    if (cache.size() > 32) cache.clear();
    return cache.emplace(key, std::move(data)).first->second;
}

MeshData buildTerrainMesh(const TerrainData& data, float textureTiling) {
    MeshData mesh;
    const int n = data.resolution;
    const float tiling = std::max(textureTiling, 1e-3f);
    mesh.vertices.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (int iz = 0; iz < n; ++iz) {
        for (int ix = 0; ix < n; ++ix) {
            Vertex v;
            v.position = data.vertex(ix, iz);
            v.normal = data.normalAt(v.position.x, v.position.z);
            v.uv = glm::vec2(v.position.x, v.position.z) / tiling;
            mesh.vertices.push_back(v);
        }
    }
    mesh.indices.reserve(static_cast<std::size_t>(n - 1) * static_cast<std::size_t>(n - 1) * 6);
    for (int iz = 0; iz + 1 < n; ++iz) {
        for (int ix = 0; ix + 1 < n; ++ix) {
            const auto v00 = static_cast<std::uint32_t>(iz * n + ix), v10 = v00 + 1;
            const auto v01 = v00 + static_cast<std::uint32_t>(n), v11 = v01 + 1;
            mesh.indices.insert(mesh.indices.end(), {v00, v01, v10, v10, v01, v11});
        }
    }
    generateTangents(mesh);
    mesh.bounds = computeBounds(mesh.vertices);
    return mesh;
}

void terrainTriangles(const TerrainData& data, const glm::mat4& world, std::vector<glm::vec3>& vertices,
                      std::vector<std::uint32_t>& indices) {
    const int n = data.resolution;
    const auto first = static_cast<std::uint32_t>(vertices.size());
    for (int iz = 0; iz < n; ++iz) {
        for (int ix = 0; ix < n; ++ix) vertices.emplace_back(world * glm::vec4(data.vertex(ix, iz), 1.0f));
    }
    for (int iz = 0; iz + 1 < n; ++iz) {
        for (int ix = 0; ix + 1 < n; ++ix) {
            const auto v00 = first + static_cast<std::uint32_t>(iz * n + ix), v10 = v00 + 1;
            const auto v01 = v00 + static_cast<std::uint32_t>(n), v11 = v01 + 1;
            indices.insert(indices.end(), {v00, v01, v10, v10, v01, v11});
        }
    }
}

std::optional<GroundHit> terrainGroundAt(const Scene& scene, float x, float z, const std::filesystem::path& projectRoot) {
    std::optional<GroundHit> best;
    for (auto [e, terrain] : scene.registry().view<TerrainComponent>().each()) {
        const glm::mat4 world = scene.worldMatrix(e);
        const glm::mat4 inverse = glm::inverse(world);
        // The point on the terrain's plane (local y = 0) below (x, z).
        const glm::vec3 local(inverse * glm::vec4(x, world[3].y, z, 1.0f));
        const float half = std::max(terrain.size, 0.01f) * 0.5f;
        if (std::abs(local.x) > half || std::abs(local.z) > half) continue;
        const auto data = terrainData(terrain, projectRoot);
        const glm::vec3 surface(world * glm::vec4(local.x, data->heightAt(local.x, local.z), local.z, 1.0f));
        const glm::vec3 normal = glm::normalize(glm::mat3(glm::transpose(inverse)) * data->normalAt(local.x, local.z));
        if (!best || surface.y > best->height) best = GroundHit{surface.y, normal};
    }
    return best;
}

std::uint64_t foliageKey(const Scene& scene, entt::entity entity, const std::filesystem::path& projectRoot) {
    const auto& f = scene.registry().get<FoliageComponent>(entity);
    std::uint64_t h = 0xF01A6Eull;
    h = mix(h, static_cast<std::uint64_t>(f.count));
    h = mixFloat(h, f.radius);
    h = mix(h, static_cast<std::uint64_t>(f.seed));
    h = mixFloat(h, f.minScale);
    h = mixFloat(h, f.maxScale);
    h = mix(h, (f.randomRotation ? 1u : 0u) | (f.onTerrain ? 2u : 0u));
    h = mixFloat(h, f.maxSlope);
    h = mixMatrix(h, scene.worldMatrix(entity));
    if (f.onTerrain) {
        for (auto [e, terrain] : scene.registry().view<TerrainComponent>().each()) {
            h = mix(h, terrainKey(terrain, projectRoot));
            h = mixMatrix(h, scene.worldMatrix(e));
        }
    }
    return h;
}

std::vector<glm::mat4> foliageInstances(const Scene& scene, entt::entity entity, const std::filesystem::path& projectRoot) {
    const auto& f = scene.registry().get<FoliageComponent>(entity);
    const glm::vec3 centre(scene.worldMatrix(entity)[3]);
    const float cosMaxSlope = std::cos(glm::radians(std::clamp(f.maxSlope, 0.0f, 90.0f)));
    const bool anyTerrain = f.onTerrain && !scene.registry().view<TerrainComponent>().empty();
    SplitMix rng{static_cast<std::uint64_t>(f.seed) * 0x2545F4914F6CDD1Dull + 1};
    std::vector<glm::mat4> out;
    const int count = std::clamp(f.count, 0, 200000);
    out.reserve(static_cast<std::size_t>(count));
    const float lo = std::min(f.minScale, f.maxScale), hi = std::max(f.minScale, f.maxScale);
    for (int i = 0; i < count; ++i) {
        // Always draw the same numbers per copy, so changing one setting does not reshuffle all.
        const float x = centre.x + (rng.uniform() * 2.0f - 1.0f) * f.radius;
        const float z = centre.z + (rng.uniform() * 2.0f - 1.0f) * f.radius;
        const float yaw = rng.uniform() * glm::two_pi<float>();
        const float scale = lo + (hi - lo) * rng.uniform();
        float y = centre.y;
        if (anyTerrain) {
            const auto ground = terrainGroundAt(scene, x, z, projectRoot);
            if (ground) {
                if (ground->normal.y < cosMaxSlope) continue;  // too steep
                y = ground->height;
            }
        }
        glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
        if (f.randomRotation) m = glm::rotate(m, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        out.push_back(glm::scale(m, glm::vec3(scale)));
    }
    return out;
}

MeshData buildWaterGrid(int cells) {
    MeshData mesh;
    const int n = std::clamp(cells, 1, 1024) + 1;
    for (int iz = 0; iz < n; ++iz) {
        for (int ix = 0; ix < n; ++ix) {
            Vertex v;
            v.position = {static_cast<float>(ix) / static_cast<float>(n - 1) - 0.5f, 0.0f,
                          static_cast<float>(iz) / static_cast<float>(n - 1) - 0.5f};
            v.uv = {v.position.x + 0.5f, v.position.z + 0.5f};
            mesh.vertices.push_back(v);
        }
    }
    for (int iz = 0; iz + 1 < n; ++iz) {
        for (int ix = 0; ix + 1 < n; ++ix) {
            const auto v00 = static_cast<std::uint32_t>(iz * n + ix), v10 = v00 + 1;
            const auto v01 = v00 + static_cast<std::uint32_t>(n), v11 = v01 + 1;
            mesh.indices.insert(mesh.indices.end(), {v00, v01, v10, v10, v01, v11});
        }
    }
    mesh.bounds = computeBounds(mesh.vertices);
    return mesh;
}

} // namespace lumora
