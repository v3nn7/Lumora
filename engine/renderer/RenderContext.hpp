#pragma once

#include "renderer/EnvironmentCache.hpp"
#include "renderer/Gpu.hpp"
#include "renderer/MeshCache.hpp"
#include "renderer/TextureCache.hpp"
#include "renderer/vulkan/Device.hpp"
#include "renderer/vulkan/Instance.hpp"

#include <nvrhi/vulkan.h>

#include <array>
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
    std::filesystem::path pipelineCacheFile;   // unused (NVRHI keeps no pipeline cache file)
};

/// Process-wide GPU context shared by every viewport: the Vulkan instance and device (chosen
/// and created by Lumora), the NVRHI device on top of them, binding layouts, the bindless
/// table, shaders, samplers and the asset caches (meshes, textures, environments).
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
    /// NVRHI device (with NVRHI's validation layer when validation is on).
    [[nodiscard]] nvrhi::IDevice* gpu() const noexcept { return gpu_; }
    /// The Vulkan backend itself (swapchain semaphores).
    [[nodiscard]] nvrhi::vulkan::IDevice* vulkanGpu() const noexcept { return vulkanGpu_; }

    [[nodiscard]] BindlessTable& bindless() noexcept { return *bindless_; }
    [[nodiscard]] ShaderLibrary& shaders() noexcept { return *shaders_; }
    /// Set 0: frame constants (b0), objects (t1), joints (t2), point lights (t3), light grid
    /// (t4) and 32 bytes of push constants.
    [[nodiscard]] nvrhi::IBindingLayout* frameLayout() const noexcept { return frameLayout_; }
    [[nodiscard]] nvrhi::GraphicsPipelineHandle createPipeline(const PipelineDesc& desc);

    /// A command list for uploads, executed at once (in queue order before later frames).
    template <class Fn>
    void upload(Fn&& record) {
        uploadList_->open();
        record(uploadList_.Get());
        uploadList_->close();
        nvrhi::ICommandList* list = uploadList_;  // (& on a handle would release it)
        gpu_->executeCommandLists(&list, 1);
    }
    /// Static buffer filled with `bytes` (vertex, index or structured data).
    /// `sharedHeap`: placed in a shared heap (never freed until shutdown); off for buffers that
    /// get replaced (procedural meshes), which get memory of their own.
    [[nodiscard]] nvrhi::BufferHandle createBuffer(std::span<const std::byte> bytes, bool vertex, bool index,
                                                   const char* name, bool sharedHeap = true);
    /// Sampled texture from mip levels (level 0 first).
    [[nodiscard]] nvrhi::TextureHandle createTexture(std::span<const std::span<const std::byte>> levels,
                                                     std::uint32_t width, std::uint32_t height, nvrhi::Format format,
                                                     const char* name);

    [[nodiscard]] MeshCache& meshes() noexcept { return *meshes_; }
    [[nodiscard]] TextureCache& textures() noexcept { return *textures_; }
    [[nodiscard]] EnvironmentCache& environments() noexcept { return *environments_; }

    /// The samplers of set 0 (gSamplers[4]); the indices below point into it.
    [[nodiscard]] const std::array<nvrhi::SamplerHandle, 4>& samplers() const noexcept { return samplers_; }
    [[nodiscard]] std::uint32_t linearClampSampler() const noexcept { return linearClampIndex_; }
    [[nodiscard]] std::uint32_t shadowCompareSampler() const noexcept { return shadowCompareIndex_; }
    /// Trilinear, anisotropic (when supported), repeating: for material textures.
    [[nodiscard]] std::uint32_t materialSampler() const noexcept { return materialSamplerIndex_; }

    [[nodiscard]] nvrhi::Format hdrFormat() const noexcept { return nvrhi::Format::RGBA16_FLOAT; }
    [[nodiscard]] nvrhi::Format depthFormat() const noexcept { return depthFormat_; }

    /// Human-readable list of all GPUs and why each is (un)suitable. Creates a temporary instance.
    [[nodiscard]] static std::string describeDevices(bool requirePresentation);

private:
    std::unique_ptr<rhi::Instance> instance_;
    std::unique_ptr<rhi::Device> device_;
    nvrhi::DeviceHandle vulkanDevice_;  // owns the backend
    nvrhi::vulkan::IDevice* vulkanGpu_ = nullptr;
    nvrhi::DeviceHandle validated_;     // NVRHI validation wrapper (optional)
    nvrhi::IDevice* gpu_ = nullptr;
    std::unique_ptr<BindlessTable> bindless_;
    std::unique_ptr<ShaderLibrary> shaders_;
    nvrhi::BindingLayoutHandle frameLayout_;
    nvrhi::CommandListHandle uploadList_;
    std::array<nvrhi::SamplerHandle, 4> samplers_;
    std::uint32_t linearClampIndex_ = 0;
    std::uint32_t shadowCompareIndex_ = 0;
    std::uint32_t materialSamplerIndex_ = 0;
    nvrhi::Format depthFormat_ = nvrhi::Format::D32;
    /// Device-local heaps static buffers are placed in (one allocation for many buffers:
    /// drivers allow only a few thousand allocations).
    struct BufferHeap {
        nvrhi::HeapHandle heap;
        std::uint64_t used = 0;
    };
    std::vector<BufferHeap> bufferHeaps_;
    [[nodiscard]] bool placeInHeap(nvrhi::IBuffer* buffer);
    std::unique_ptr<MeshCache> meshes_;
    std::unique_ptr<TextureCache> textures_;
    std::unique_ptr<EnvironmentCache> environments_;
};

} // namespace lumora::render
