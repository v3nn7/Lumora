#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumora::rhi {

class Device;

/// Loads precompiled SPIR-V modules (built from shaders/*.hlsl) and caches them by name.
class ShaderLibrary {
public:
    ShaderLibrary(const Device& device, std::filesystem::path directory);

    /// Returns the module for e.g. "mesh.vert". Throws VulkanError with the path if missing/invalid.
    [[nodiscard]] VkShaderModule get(const std::string& name);
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    void clear() { modules_.clear(); }

private:
    const Device& device_;
    std::filesystem::path directory_;
    std::unordered_map<std::string, UniqueShaderModule> modules_;
};

/// VkPipelineCache persisted to disk. The driver validates the header (vendor, device,
/// cache UUID); we additionally check it ourselves so stale caches are discarded cleanly.
class PipelineCache {
public:
    PipelineCache(const Device& device, std::filesystem::path file);
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    [[nodiscard]] VkPipelineCache handle() const noexcept { return cache_; }
    void save() const;

private:
    const Device& device_;
    std::filesystem::path file_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
};

enum class BlendMode { Opaque, Alpha, Additive };

struct GraphicsPipelineDesc {
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;  // optional (depth-only passes)
    const char* vertexEntry = "VSMain";
    const char* fragmentEntry = "PSMain";
    bool meshVertexInput = false;  // binds lumora::Vertex (position, normal, uv)
    bool positionOnly = false;     // with meshVertexInput: only location 0 (depth-only passes)
    bool skinVertexInput = false;  // with meshVertexInput: binding 1 = lumora::SkinVertex (locations 4, 5)
    bool overlayVertexInput = false;  // render::OverlayVertex: float3 position + RGBA8 colour
    bool hudVertexInput = false;      // render::HudVertex: float2 position, float2 uv, float4 colour
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed-Z
    bool depthBias = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    BlendMode blend = BlendMode::Opaque;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const char* debugName = nullptr;
};

/// Builds a dynamic-rendering graphics pipeline (viewport/scissor are dynamic state).
[[nodiscard]] UniquePipeline createGraphicsPipeline(const Device& device, const PipelineCache& cache,
                                                    const GraphicsPipelineDesc& desc);

} // namespace lumora::rhi
