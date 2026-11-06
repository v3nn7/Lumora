#pragma once

#include "renderer/GpuProfiler.hpp"
#include "renderer/TransientImagePool.hpp"
#include "renderer/vulkan/Resources.hpp"

#include <array>
#include <memory>
#include <vector>

namespace lumora::render {

/// A CPU-written buffer that grows as needed (frame data: objects, lights, vertices).
struct DynamicBuffer {
    nvrhi::BufferHandle buffer;
    std::uint64_t capacity = 0;  // bytes

    enum class Kind : std::uint8_t { Structured, Vertex, Constant };
    /// Makes room for `bytes` (at least one element) and writes them.
    nvrhi::IBuffer* write(nvrhi::IDevice* device, std::span<const std::byte> bytes, Kind kind, std::uint32_t stride,
                          const char* name);
};

/// Everything owned by one frame-in-flight slot. Safe to change only after `done` signalled.
struct FrameResources {
    std::vector<nvrhi::CommandListHandle> lists;
    std::size_t usedLists = 0;
    nvrhi::EventQueryHandle done;
    bool pending = false;
    rhi::UniqueSemaphore acquired;  // swapchain image acquire

    DynamicBuffer constants;  // FrameConstants
    DynamicBuffer objects;    // GpuObject[]
    DynamicBuffer joints;     // float4x4[] skinning palettes
    DynamicBuffer pointLights;
    DynamicBuffer lightGrid;
    DynamicBuffer overlayVertices;
    DynamicBuffer hudVertices;
    DynamicBuffer particleVertices;
    nvrhi::BindingSetHandle bindings;  // set 0 over the buffers above
    std::array<nvrhi::IBuffer*, 5> bound{};

    std::unique_ptr<TransientImagePool> transients;
    std::unique_ptr<GpuProfiler> profiler;

    /// An open command list of this frame (reused across frames).
    nvrhi::CommandListHandle nextList(nvrhi::IDevice* device);
    /// Set 0 for this frame's buffers (rebuilt when one of them was reallocated).
    nvrhi::IBindingSet* bindingSet(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout,
                                   const std::array<nvrhi::SamplerHandle, 4>& samplers);
};

} // namespace lumora::render
