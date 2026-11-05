#include "renderer/ParallelCommandPools.hpp"

#include "renderer/vulkan/Device.hpp"

namespace lumora::render {

ParallelCommandPools::ParallelCommandPools(const rhi::Device& device, std::uint32_t chunks) : device_(device.handle()) {
    pools_.reserve(chunks);
    for (std::uint32_t i = 0; i < chunks; ++i) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = device.graphicsFamily();
        VkCommandPool pool = VK_NULL_HANDLE;
        LUMORA_VK_CHECK(vkCreateCommandPool(device_, &info, nullptr, &pool));
        pools_.emplace_back(device_, pool);
    }
}

void ParallelCommandPools::reset() {
    for (const auto& pool : pools_) {
        LUMORA_VK_CHECK(vkResetCommandPool(device_, pool.get(), 0));
    }
}

VkCommandBuffer ParallelCommandPools::allocate(std::uint32_t chunk) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pools_.at(chunk).get();
    alloc.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, &cmd));
    return cmd;
}

} // namespace lumora::render
