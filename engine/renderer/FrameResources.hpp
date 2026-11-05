#pragma once

#include "renderer/GpuProfiler.hpp"
#include "renderer/TransientImagePool.hpp"
#include "renderer/vulkan/Resources.hpp"

#include <memory>

namespace lumora::render {

/// Everything owned by one frame-in-flight slot. Safe to mutate only after `fence` signalled.
struct FrameResources {
    rhi::UniqueCommandPool commandPool;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    rhi::UniqueFence fence;
    rhi::UniqueSemaphore acquired;
    rhi::Buffer constants;           // FrameConstants (host-visible UBO)
    rhi::Buffer objects;             // GpuObject[] (host-visible SSBO)
    std::uint32_t objectCapacity = 0;
    rhi::Buffer overlayVertices;     // render::OverlayVertex[] (host-visible vertex buffer)
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::unique_ptr<TransientImagePool> transients;
    std::unique_ptr<GpuProfiler> profiler;
    bool pending = false;            // submitted and fence not yet waited
};

} // namespace lumora::render
