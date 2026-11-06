#pragma once

// The renderer's GPU layer is NVRHI. This header collects what every part of the renderer
// needs from it: handles, bindless descriptors, shaders, pipelines and uploads.
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lumora::render {

[[nodiscard]] bool isSrgbFormat(nvrhi::Format format) noexcept;

/// Descriptor table of set 1: every sampled texture at a stable index the shaders receive in
/// constants (binding 0). Thread-safe. (Samplers are a small fixed array in set 0.)
class BindlessTable {
public:
    static constexpr std::uint32_t kCapacity = 16384;

    explicit BindlessTable(nvrhi::IDevice* device);
    [[nodiscard]] nvrhi::IBindingLayout* layout() const noexcept { return layout_; }
    [[nodiscard]] nvrhi::IDescriptorTable* table() const noexcept { return table_; }

    /// Registers a texture (all mips; `viewFormat` reinterprets, e.g. sRGB). The caller keeps
    /// it alive until release and releases only once the GPU no longer uses the index.
    [[nodiscard]] std::uint32_t registerTexture(nvrhi::ITexture* texture, nvrhi::Format viewFormat = nvrhi::Format::UNKNOWN);
    void releaseTexture(std::uint32_t index);

private:
    nvrhi::IDevice* device_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::DescriptorTableHandle table_;
    std::mutex mutex_;
    std::vector<std::uint32_t> freeTextures_;
    std::uint32_t nextTexture_ = 0;
};

/// SPIR-V shaders from the build's shader folder, created once per (file, entry point).
class ShaderLibrary {
public:
    ShaderLibrary(nvrhi::IDevice* device, std::filesystem::path directory);
    /// `name` is the file without ".spv", e.g. "mesh.vert"; the stage follows its suffix.
    [[nodiscard]] nvrhi::IShader* get(std::string_view name, const char* entry);

private:
    nvrhi::IDevice* device_;
    std::filesystem::path directory_;
    std::mutex mutex_;
    std::unordered_map<std::string, nvrhi::ShaderHandle> shaders_;
};

enum class BlendMode : std::uint8_t { Opaque, Alpha, Additive, Multiply };

/// Vertex streams the pipelines read (see shaders: [[vk::location]] numbers in order).
enum class VertexInput : std::uint8_t {
    None,
    Mesh,               // lumora::Vertex: position, normal, uv, tangent
    MeshPosition,       // position only (depth-only passes)
    MeshSkinned,        // Vertex + SkinVertex (stream 1): joints, weights
    MeshSkinnedPosition,
    Overlay,            // render::OverlayVertex: float3 position, RGBA8 colour
    Hud,                // render::HudVertex: float2 position, float2 uv, float4 colour
    Particle,           // ParticleVertex: float3 position, float2 corner, float4 colour
};

struct PipelineDesc {
    std::string name;
    std::string vertexShader;  // e.g. "mesh.vert"
    const char* vertexEntry = "VSMain";
    std::string pixelShader;   // empty for depth-only passes
    const char* pixelEntry = "PSMain";
    VertexInput input = VertexInput::None;
    nvrhi::RasterCullMode cull = nvrhi::RasterCullMode::None;
    bool depthTest = false;
    bool depthWrite = false;
    nvrhi::ComparisonFunc depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;  // reversed-Z
    int depthBias = 0;
    float slopeScaledDepthBias = 0.0f;
    BlendMode blend = BlendMode::Opaque;
    std::vector<nvrhi::Format> colorFormats;
    nvrhi::Format depthFormat = nvrhi::Format::UNKNOWN;
};

/// Writes `bytes` into a CPU-writable buffer (frame data; mapped, copied, unmapped).
void writeMapped(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, std::span<const std::byte> bytes);

/// Mip chain of an RGBA8 image: each level a 2x2 box average (in linear light for sRGB data).
[[nodiscard]] std::vector<std::vector<std::uint8_t>> buildMipChain(std::span<const std::uint8_t> rgba,
                                                                   std::uint32_t width, std::uint32_t height,
                                                                   bool srgb);

} // namespace lumora::render
