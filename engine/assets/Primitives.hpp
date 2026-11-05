#pragma once

#include "assets/MeshData.hpp"

#include <cstdint>
#include <string_view>

namespace lumora {

/// Built-in procedural meshes. Referenced by MeshRendererComponent until imported assets exist.
enum class MeshPrimitive : std::int32_t { Cube = 0, Sphere = 1, Plane = 2, Cylinder = 3, Count };

[[nodiscard]] std::string_view toString(MeshPrimitive primitive) noexcept;

[[nodiscard]] MeshData makeCube(float size = 1.0f);
[[nodiscard]] MeshData makeUvSphere(float radius = 0.5f, std::uint32_t segments = 48, std::uint32_t rings = 24);
[[nodiscard]] MeshData makePlane(float size = 1.0f, std::uint32_t subdivisions = 1);
[[nodiscard]] MeshData makeCylinder(float radius = 0.5f, float height = 1.0f, std::uint32_t segments = 48);
[[nodiscard]] MeshData makePrimitive(MeshPrimitive primitive);

} // namespace lumora
