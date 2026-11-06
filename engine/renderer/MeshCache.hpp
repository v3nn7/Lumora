#pragma once

#include "assets/MeshData.hpp"
#include "assets/Primitives.hpp"
#include "renderer/Gpu.hpp"

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lumora {
class AssetLibrary;
}


namespace lumora::render {

struct GpuMesh {
    nvrhi::BufferHandle vertices;
    nvrhi::BufferHandle indices;
    std::uint32_t indexCount = 0;
    Aabb bounds;
    nvrhi::BufferHandle skin;  // lumora::SkinVertex per vertex, when the mesh is skinned
    [[nodiscard]] bool skinned() const noexcept { return skin != nullptr; }
};

class RenderContext;

/// GPU meshes: built-in primitives (uploaded at startup) and imported mesh assets (uploaded
/// on first use, non-blocking). Asset meshes are keyed by absolute path so projects never
/// collide; entries live until the RenderContext is destroyed.
class MeshCache {
public:
    explicit MeshCache(RenderContext& context);
    [[nodiscard]] const GpuMesh& primitive(MeshPrimitive p) const;
    /// nullptr if the reference cannot be resolved/loaded (reported once by the library) or,
    /// when not blocking, while it is still loading in the background.
    [[nodiscard]] const GpuMesh* asset(AssetLibrary& library, const std::string& key, bool block);
    [[nodiscard]] std::size_t assetCount() const noexcept { return assets_.size(); }
    /// Lets meshes that failed to load be tried again.
    void forgetFailures() { failed_.clear(); }
    /// A mesh made by code (terrain, water grid) in `slot`, rebuilt with `build` when `key` changes.
    [[nodiscard]] const GpuMesh* procedural(const std::string& slot, std::uint64_t key, const std::function<MeshData()>& build);
    /// Drops the meshes of a model file (absolute, generic form); the GPU must be idle.
    std::size_t invalidateFile(const std::string& file);

private:
    RenderContext& ctx_;
    std::array<GpuMesh, static_cast<std::size_t>(MeshPrimitive::Count)> primitives_;
    std::unordered_map<std::string, GpuMesh> assets_;
    std::unordered_set<std::string> failed_;
    struct Procedural {
        std::uint64_t key = 0;
        GpuMesh mesh;
    };
    std::unordered_map<std::string, Procedural> procedural_;
};

} // namespace lumora::render
