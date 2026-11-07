#include "renderer/FrameResources.hpp"

#include "renderer/GpuTypes.hpp"

#include <algorithm>

namespace lumora::render {

nvrhi::IBuffer* DynamicBuffer::write(nvrhi::IDevice* device, std::span<const std::byte> bytes, Kind kind,
                                     std::uint32_t stride, const char* name) {
    const std::uint64_t needed = std::max<std::uint64_t>(bytes.size(), stride);
    if (!buffer || capacity < needed) {
        std::uint64_t grown = std::max<std::uint64_t>(capacity, kind == Kind::Constant ? needed : 64 * 1024);
        while (grown < needed) grown *= 2;
        nvrhi::BufferDesc desc;
        desc.byteSize = grown;
        desc.debugName = name;
        desc.cpuAccess = nvrhi::CpuAccessMode::Write;
        desc.isVertexBuffer = kind == Kind::Vertex;
        desc.isConstantBuffer = kind == Kind::Constant;
        desc.structStride = kind == Kind::Structured ? stride : 0;
        desc.initialState = kind == Kind::Vertex     ? nvrhi::ResourceStates::VertexBuffer
                            : kind == Kind::Constant ? nvrhi::ResourceStates::ConstantBuffer
                                                     : nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        buffer = device->createBuffer(desc);
        if (!buffer) throw rhi::VulkanError(std::string("could not create ") + name, VK_ERROR_OUT_OF_DEVICE_MEMORY);
        capacity = grown;
    }
    writeMapped(device, buffer, bytes);
    return buffer;
}

nvrhi::CommandListHandle FrameResources::nextList(nvrhi::IDevice* device) {
    // Not "immediate": several of them are recorded at once (parallel passes), which NVRHI allows
    // only for deferred command lists.
    if (usedLists == lists.size()) {
        lists.push_back(device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
    }
    nvrhi::CommandListHandle list = lists[usedLists++];
    list->open();
    return list;
}

nvrhi::IBindingSet* FrameResources::bindingSet(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout,
                                               const std::array<nvrhi::SamplerHandle, 4>& samplers) {
    const std::array<nvrhi::IBuffer*, 5> now{constants.buffer, objects.buffer, joints.buffer, pointLights.buffer,
                                            lightGrid.buffer};
    if (!bindings || now != bound) {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constants.buffer),
                         nvrhi::BindingSetItem::StructuredBuffer_SRV(1, objects.buffer),
                         nvrhi::BindingSetItem::StructuredBuffer_SRV(2, joints.buffer),
                         nvrhi::BindingSetItem::StructuredBuffer_SRV(3, pointLights.buffer),
                         nvrhi::BindingSetItem::StructuredBuffer_SRV(4, lightGrid.buffer),
                         nvrhi::BindingSetItem::PushConstants(1, sizeof(PushConstants))};
        const bool d3d12 = device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;
        for (std::uint32_t i = 0; i < samplers.size(); ++i) {
            desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(5, samplers[i]).setArrayElement(i));
            // D3D12: again as the comparison samplers (s9, see RenderContext's frame layout).
            if (d3d12) desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(9, samplers[i]).setArrayElement(i));
        }
        bindings = device->createBindingSet(desc, layout);
        if (!bindings) throw rhi::VulkanError("could not create the frame binding set", VK_ERROR_INITIALIZATION_FAILED);
        bound = now;
    }
    return bindings;
}

} // namespace lumora::render
