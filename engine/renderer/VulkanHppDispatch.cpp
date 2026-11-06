// NVRHI's Vulkan backend uses vulkan.hpp with a dynamic dispatcher; a static NVRHI build expects
// the application to define its storage in exactly one file.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace lumora::render {

/// Points the vulkan.hpp dispatcher NVRHI uses at our instance and device.
void initVulkanHppDispatcher(VkInstance instance, VkDevice device) {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance), vkGetInstanceProcAddr, vk::Device(device));
}

} // namespace lumora::render
