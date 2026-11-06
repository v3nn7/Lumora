#include "renderer/MeshCache.hpp"

#include "assets/AssetLibrary.hpp"
#include "assets/MeshLod.hpp"
#include "core/Log.hpp"
#include "renderer/RenderContext.hpp"

#include <format>
#include <span>

namespace lumora::render {

MeshCache::MeshCache(RenderContext& context) : ctx_(context) {
    for (std::size_t i = 0; i < primitives_.size(); ++i) {
        const auto prim = static_cast<MeshPrimitive>(i);
        const MeshData data = makePrimitive(prim);
        GpuMesh& mesh = primitives_[i];
        const std::string vbName = std::format("{}.vertices", toString(prim));
        const std::string ibName = std::format("{}.indices", toString(prim));
        mesh.vertices = ctx_.createBuffer(std::as_bytes(std::span(data.vertices)), true, false, vbName.c_str());
        mesh.indices = ctx_.createBuffer(std::as_bytes(std::span(data.indices)), false, true, ibName.c_str());
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
    gpu.vertices = ctx_.createBuffer(std::as_bytes(std::span(data.vertices)), true, false, (name + ".vertices").c_str());
    gpu.indices = ctx_.createBuffer(std::as_bytes(std::span(data.indices)), false, true, (name + ".indices").c_str());
    gpu.indexCount = static_cast<std::uint32_t>(data.indices.size());
    gpu.bounds = data.bounds;
    if (!data.skin.empty() && data.skin.size() == data.vertices.size()) {
        gpu.skin = ctx_.createBuffer(std::as_bytes(std::span(data.skin)), true, false, (name + ".skin").c_str());
    }
    // Levels of detail share the vertex (and skin) buffers; each has its own indices.
    for (const MeshLod& lod : buildLods(data)) {
        GpuMesh level;
        level.vertices = gpu.vertices;
        level.skin = gpu.skin;
        level.bounds = gpu.bounds;
        level.indexCount = static_cast<std::uint32_t>(lod.indices.size());
        const std::string lodName = std::format("{}.lod{}.indices", name, gpu.lods.size() + 1);
        level.indices = ctx_.createBuffer(std::as_bytes(std::span(lod.indices)), false, true, lodName.c_str());
        gpu.lods.push_back(std::move(level));
    }
    log::debug("Renderer", "uploaded mesh '{}' ({} triangles, {} levels of detail)", key, gpu.indexCount / 3, gpu.lods.size());
    return &assets_.emplace(*canonical, std::move(gpu)).first->second;
}

const GpuMesh* MeshCache::procedural(const std::string& slot, std::uint64_t key, const std::function<MeshData()>& build) {
    auto it = procedural_.find(slot);
    if (it != procedural_.end() && it->second.key == key) return &it->second.mesh;
    const MeshData data = build();
    if (data.indices.empty()) return nullptr;
    GpuMesh gpu;
    gpu.vertices = ctx_.createBuffer(std::as_bytes(std::span(data.vertices)), true, false, (slot + ".vertices").c_str(), false);
    gpu.indices = ctx_.createBuffer(std::as_bytes(std::span(data.indices)), false, true, (slot + ".indices").c_str(), false);
    gpu.indexCount = static_cast<std::uint32_t>(data.indices.size());
    gpu.bounds = data.bounds;
    // The old buffers stay alive until the GPU is done with the frames that use them (NVRHI).
    Procedural& p = procedural_[slot];
    p.key = key;
    p.mesh = std::move(gpu);
    return &p.mesh;
}

const GpuMesh& MeshCache::primitive(MeshPrimitive p) const {
    const auto i = static_cast<std::size_t>(p);
    return primitives_[i < primitives_.size() ? i : 0];
}

} // namespace lumora::render
