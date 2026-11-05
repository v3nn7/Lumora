#include "renderer/MeshCache.hpp"

#include "core/Log.hpp"
#include "renderer/vulkan/UploadQueue.hpp"

#include <format>
#include <span>

namespace lumora::render {

MeshCache::MeshCache(rhi::UploadQueue& uploads) {
    for (std::size_t i = 0; i < primitives_.size(); ++i) {
        const auto prim = static_cast<MeshPrimitive>(i);
        const MeshData data = makePrimitive(prim);
        GpuMesh& mesh = primitives_[i];
        const std::string vbName = std::format("{}.vertices", toString(prim));
        const std::string ibName = std::format("{}.indices", toString(prim));
        mesh.vertices = uploads.uploadBuffer(std::as_bytes(std::span(data.vertices)),
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbName.c_str());
        mesh.indices = uploads.uploadBuffer(std::as_bytes(std::span(data.indices)),
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ibName.c_str());
        mesh.indexCount = static_cast<std::uint32_t>(data.indices.size());
        mesh.bounds = data.bounds;
    }
    log::debug("Renderer", "uploaded {} primitive meshes", primitives_.size());
}

const GpuMesh& MeshCache::primitive(MeshPrimitive p) const {
    const auto i = static_cast<std::size_t>(p);
    return primitives_[i < primitives_.size() ? i : 0];
}

} // namespace lumora::render
