#pragma once

#include "assets/MeshData.hpp"
#include "assets/Primitives.hpp"
#include "renderer/vulkan/Resources.hpp"

#include <array>

namespace lumora::rhi {
class UploadQueue;
}

namespace lumora::render {

struct GpuMesh {
    rhi::Buffer vertices;
    rhi::Buffer indices;
    std::uint32_t indexCount = 0;
    Aabb bounds;
};

/// GPU copies of the built-in primitive meshes, uploaded once at startup.
class MeshCache {
public:
    explicit MeshCache(rhi::UploadQueue& uploads);
    [[nodiscard]] const GpuMesh& primitive(MeshPrimitive p) const;

private:
    std::array<GpuMesh, static_cast<std::size_t>(MeshPrimitive::Count)> primitives_;
};

} // namespace lumora::render
