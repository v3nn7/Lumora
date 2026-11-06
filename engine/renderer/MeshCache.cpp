#include "renderer/MeshCache.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/Log.hpp"
#include "renderer/vulkan/UploadQueue.hpp"

#include <format>
#include <span>

namespace lumora::render {

MeshCache::MeshCache(rhi::UploadQueue& uploads) : uploads_(uploads) {
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

std::size_t MeshCache::invalidateFile(const std::string& file) {
    const std::string prefix = file + "#";
    std::erase_if(failed_, [&](const std::string& key) { return key.starts_with(prefix); });
    return std::erase_if(assets_, [&](const auto& entry) { return entry.first.starts_with(prefix); });
}

const GpuMesh* MeshCache::asset(AssetLibrary& library, const std::string& key, bool block) {
    auto canonical = library.canonicalKey(key);
    if (!canonical) return nullptr;
    if (auto it = assets_.find(*canonical); it != assets_.end()) return &it->second;
    if (failed_.contains(*canonical)) return nullptr;
    const ImportedMesh* mesh = nullptr;
    if (block) {
        auto loaded = library.mesh(key);
        if (loaded) mesh = *loaded;
    } else {
        const auto request = library.requestMesh(key);
        if (request.state == AssetState::Pending) return nullptr;  // try again next frame
        mesh = request.value;
    }
    if (mesh == nullptr || mesh->data.indices.empty()) {
        if (mesh != nullptr) log::warn("Renderer", "mesh '{}' has no triangles", key);
        failed_.insert(*canonical);
        return nullptr;
    }
    const MeshData& data = mesh->data;
    GpuMesh gpu;
    const std::string name = mesh->name;
    gpu.vertices = uploads_.uploadBuffer(std::as_bytes(std::span(data.vertices)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         (name + ".vertices").c_str());
    gpu.indices = uploads_.uploadBuffer(std::as_bytes(std::span(data.indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                        (name + ".indices").c_str());
    gpu.indexCount = static_cast<std::uint32_t>(data.indices.size());
    gpu.bounds = data.bounds;
    if (!data.skin.empty() && data.skin.size() == data.vertices.size()) {
        gpu.skin = uploads_.uploadBuffer(std::as_bytes(std::span(data.skin)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         (name + ".skin").c_str());
    }
    log::debug("Renderer", "uploaded mesh '{}' ({} triangles)", key, gpu.indexCount / 3);
    return &assets_.emplace(*canonical, std::move(gpu)).first->second;
}

const GpuMesh& MeshCache::primitive(MeshPrimitive p) const {
    const auto i = static_cast<std::size_t>(p);
    return primitives_[i < primitives_.size() ? i : 0];
}

} // namespace lumora::render
