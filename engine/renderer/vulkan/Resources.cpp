#include "renderer/vulkan/Resources.hpp"

#include "renderer/vulkan/Device.hpp"

namespace lumora::rhi {

UniqueSemaphore createBinarySemaphore(const Device& device, const char* name) {
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateSemaphore(device.handle(), &info, nullptr, &s));
    if (name != nullptr) device.setName(VK_OBJECT_TYPE_SEMAPHORE, s, name);
    return {device.handle(), s};
}

UniqueSemaphore createTimelineSemaphore(const Device& device, std::uint64_t initial, const char* name) {
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initial;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    VkSemaphore s = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateSemaphore(device.handle(), &info, nullptr, &s));
    if (name != nullptr) device.setName(VK_OBJECT_TYPE_SEMAPHORE, s, name);
    return {device.handle(), s};
}

UniqueFence createFence(const Device& device, bool signaled, const char* name) {
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (signaled) info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence f = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateFence(device.handle(), &info, nullptr, &f));
    if (name != nullptr) device.setName(VK_OBJECT_TYPE_FENCE, f, name);
    return {device.handle(), f};
}

} // namespace lumora::rhi
