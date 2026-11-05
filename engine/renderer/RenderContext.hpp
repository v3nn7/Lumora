#pragma once

#include "renderer/MeshCache.hpp"
#include "renderer/vulkan/BindlessTable.hpp"
#include "renderer/vulkan/Device.hpp"
#include "renderer/vulkan/Instance.hpp"
#include "renderer/vulkan/Pipeline.hpp"
#include "renderer/vulkan/UploadQueue.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lumora::render {

struct RenderContextConfig {
    bool enableValidation = false;
    /// Platform surface extensions needed by the windowing layer (empty for headless).
    std::vector<std::string> instanceExtensions;
    std::optional<std::uint32_t> gpuIndex;
    bool requirePresentation = true;
    std::filesystem::path shaderDirectory;     // default: paths::shaderDir()
    std::filesystem::path pipelineCacheFile;   // default: <user cache>/pipeline_cache.bin
};

/// Process-wide GPU context shared by every viewport: instance, device, allocator, bindless
/// table, pipeline layout, shaders, pipeline cache, samplers and built-in meshes.
/// Construction throws rhi::UnsupportedHardwareError / rhi::VulkanError with a clear message.
class RenderContext {
public:
    explicit RenderContext(const RenderContextConfig& config);
    ~RenderContext();
    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    [[nodiscard]] rhi::Instance& instance() noexcept { return *instance_; }
    [[nodiscard]] rhi::Device& device() noexcept { return *device_; }
    [[nodiscard]] const rhi::DeviceCapabilities& caps() const noexcept { return device_->caps(); }
    [[nodiscard]] rhi::UploadQueue& uploads() noexcept { return *uploads_; }
    [[nodiscard]] rhi::BindlessTable& bindless() noexcept { return *bindless_; }
    [[nodiscard]] rhi::ShaderLibrary& shaders() noexcept { return *shaders_; }
    [[nodiscard]] rhi::PipelineCache& pipelineCache() noexcept { return *pipelineCache_; }
    [[nodiscard]] MeshCache& meshes() noexcept { return *meshes_; }

    [[nodiscard]] VkDescriptorSetLayout frameSetLayout() const noexcept { return frameSetLayout_.get(); }
    [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept { return pipelineLayout_.get(); }
    [[nodiscard]] std::uint32_t linearClampSampler() const noexcept { return linearClampIndex_; }
    [[nodiscard]] VkFormat hdrFormat() const noexcept { return hdrFormat_; }
    [[nodiscard]] VkFormat depthFormat() const noexcept { return depthFormat_; }

    /// Human-readable list of all GPUs and why each is (un)suitable. Creates a temporary instance.
    [[nodiscard]] static std::string describeDevices(bool requirePresentation);

private:
    std::unique_ptr<rhi::Instance> instance_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<rhi::UploadQueue> uploads_;
    std::unique_ptr<rhi::BindlessTable> bindless_;
    std::unique_ptr<rhi::ShaderLibrary> shaders_;
    std::unique_ptr<rhi::PipelineCache> pipelineCache_;
    std::unique_ptr<MeshCache> meshes_;
    rhi::UniqueDescriptorSetLayout frameSetLayout_;
    rhi::UniquePipelineLayout pipelineLayout_;
    rhi::UniqueSampler linearClamp_;
    std::uint32_t linearClampIndex_ = 0;
    VkFormat hdrFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
};

} // namespace lumora::render
