#include "assets/Primitives.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <limits>

namespace lumora {

Aabb computeBounds(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) {
        return {};
    }
    Aabb box{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())};
    for (const Vertex& v : vertices) {
        box.min = glm::min(box.min, v.position);
        box.max = glm::max(box.max, v.position);
    }
    return box;
}

std::string_view toString(MeshPrimitive primitive) noexcept {
    switch (primitive) {
        case MeshPrimitive::Cube: return "Cube";
        case MeshPrimitive::Sphere: return "Sphere";
        case MeshPrimitive::Plane: return "Plane";
        case MeshPrimitive::Cylinder: return "Cylinder";
        case MeshPrimitive::Count: break;
    }
    return "Unknown";
}

MeshData makeCube(float size) {
    const float h = size * 0.5f;
    struct Face {
        glm::vec3 normal, u, v;
    };
    // u x v == normal, which yields counter-clockwise winding seen from outside.
    constexpr Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);
    for (const Face& f : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        const glm::vec3 c = f.normal * h;
        mesh.vertices.push_back({c - f.u * h - f.v * h, f.normal, {0, 1}});
        mesh.vertices.push_back({c + f.u * h - f.v * h, f.normal, {1, 1}});
        mesh.vertices.push_back({c + f.u * h + f.v * h, f.normal, {1, 0}});
        mesh.vertices.push_back({c - f.u * h + f.v * h, f.normal, {0, 0}});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    mesh.bounds = computeBounds(mesh.vertices);
    return mesh;
}

MeshData makeUvSphere(float radius, std::uint32_t segments, std::uint32_t rings) {
    segments = std::max(segments, 3u);
    rings = std::max(rings, 2u);
    MeshData mesh;
    mesh.vertices.reserve(static_cast<std::size_t>(segments + 1) * (rings + 1));
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = v * glm::pi<float>(); // 0 at +Y pole
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float theta = u * glm::two_pi<float>();
            const glm::vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta)};
            mesh.vertices.push_back({n * radius, n, {u, v}});
        }
    }
    const std::uint32_t stride = segments + 1;
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < segments; ++s) {
            const std::uint32_t a = r * stride + s;
            const std::uint32_t b = a + stride;
            if (r != 0) {
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1});
            }
            if (r != rings - 1) {
                mesh.indices.insert(mesh.indices.end(), {a + 1, b, b + 1});
            }
        }
    }
    mesh.bounds = computeBounds(mesh.vertices);
    return mesh;
}

MeshData makePlane(float size, std::uint32_t subdivisions) {
    subdivisions = std::max(subdivisions, 1u);
    MeshData mesh;
    const float h = size * 0.5f;
    const std::uint32_t n = subdivisions + 1;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(subdivisions);
            const float v = static_cast<float>(z) / static_cast<float>(subdivisions);
            mesh.vertices.push_back({{-h + u * size, 0.0f, -h + v * size}, {0, 1, 0}, {u * size, v * size}});
        }
    }
    for (std::uint32_t z = 0; z < subdivisions; ++z) {
        for (std::uint32_t x = 0; x < subdivisions; ++x) {
            const std::uint32_t a = z * n + x;
            const std::uint32_t b = a + n;
            // a(x,z) -> b(x,z+1) -> a+1(x+1,z) is counter-clockwise seen from +Y.
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    mesh.bounds = computeBounds(mesh.vertices);
    return mesh;
}

MeshData makeCylinder(float radius, float height, std::uint32_t segments) {
    segments = std::max(segments, 3u);
    MeshData mesh;
    const float h = height * 0.5f;
    // Side
    for (std::uint32_t s = 0; s <= segments; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(segments);
        const float theta = u * glm::two_pi<float>();
        const glm::vec3 n{std::cos(theta), 0.0f, -std::sin(theta)};
        mesh.vertices.push_back({n * radius + glm::vec3(0, h, 0), n, {u, 0}});
        mesh.vertices.push_back({n * radius - glm::vec3(0, h, 0), n, {u, 1}});
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        const std::uint32_t top0 = s * 2, bot0 = top0 + 1, top1 = top0 + 2, bot1 = top0 + 3;
        mesh.indices.insert(mesh.indices.end(), {top0, bot0, bot1, top0, bot1, top1});
    }
    // Caps
    for (int cap = 0; cap < 2; ++cap) {
        const float y = cap == 0 ? h : -h;
        const glm::vec3 n{0.0f, cap == 0 ? 1.0f : -1.0f, 0.0f};
        const auto center = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({{0, y, 0}, n, {0.5f, 0.5f}});
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float theta = static_cast<float>(s) / static_cast<float>(segments) * glm::two_pi<float>();
            const glm::vec3 p{std::cos(theta) * radius, y, -std::sin(theta) * radius};
            mesh.vertices.push_back({p, n, {0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)}});
        }
        for (std::uint32_t s = 0; s < segments; ++s) {
            const std::uint32_t a = center + 1 + s;
            if (cap == 0) {
                mesh.indices.insert(mesh.indices.end(), {center, a, a + 1});
            } else {
                mesh.indices.insert(mesh.indices.end(), {center, a + 1, a});
            }
        }
    }
    mesh.bounds = computeBounds(mesh.vertices);
    return mesh;
}

MeshData makePrimitive(MeshPrimitive primitive) {
    switch (primitive) {
        case MeshPrimitive::Cube: return makeCube();
        case MeshPrimitive::Sphere: return makeUvSphere();
        case MeshPrimitive::Plane: return makePlane(1.0f, 1);
        case MeshPrimitive::Cylinder: return makeCylinder();
        case MeshPrimitive::Count: break;
    }
    return makeCube();
}

} // namespace lumora
