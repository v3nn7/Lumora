#pragma once

#include "renderer/vulkan/VulkanCommon.hpp"

#include <cstdint>
#include <utility>

namespace lumora::rhi {

class Device;

/// Generic owning wrapper for simple device-level handles destroyed with vkDestroyXxx(device, h, nullptr).
template <class Handle, void (*Destroy)(VkDevice, Handle, const VkAllocationCallbacks*)>
class DeviceHandle {
public:
    DeviceHandle() = default;
    DeviceHandle(VkDevice device, Handle handle) noexcept : device_(device), handle_(handle) {}
    ~DeviceHandle() { reset(); }
    DeviceHandle(DeviceHandle&& o) noexcept
        : device_(std::exchange(o.device_, VK_NULL_HANDLE)), handle_(std::exchange(o.handle_, VK_NULL_HANDLE)) {}
    DeviceHandle& operator=(DeviceHandle&& o) noexcept {
        if (this != &o) {
            reset();
            device_ = std::exchange(o.device_, VK_NULL_HANDLE);
            handle_ = std::exchange(o.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }
    DeviceHandle(const DeviceHandle&) = delete;
    DeviceHandle& operator=(const DeviceHandle&) = delete;

    void reset() noexcept {
        if (handle_ != VK_NULL_HANDLE) Destroy(device_, handle_, nullptr);
        handle_ = VK_NULL_HANDLE;
    }
    [[nodiscard]] Handle get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != VK_NULL_HANDLE; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    Handle handle_ = VK_NULL_HANDLE;
};

using UniqueSemaphore = DeviceHandle<VkSemaphore, vkDestroySemaphore>;
using UniqueFence = DeviceHandle<VkFence, vkDestroyFence>;
using UniqueCommandPool = DeviceHandle<VkCommandPool, vkDestroyCommandPool>;
using UniqueSampler = DeviceHandle<VkSampler, vkDestroySampler>;
using UniqueDescriptorPool = DeviceHandle<VkDescriptorPool, vkDestroyDescriptorPool>;
using UniqueDescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>;
using UniquePipelineLayout = DeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout>;
using UniquePipeline = DeviceHandle<VkPipeline, vkDestroyPipeline>;
using UniqueShaderModule = DeviceHandle<VkShaderModule, vkDestroyShaderModule>;
using UniqueQueryPool = DeviceHandle<VkQueryPool, vkDestroyQueryPool>;

[[nodiscard]] UniqueSemaphore createBinarySemaphore(const Device& device, const char* name = nullptr);
[[nodiscard]] UniqueSemaphore createTimelineSemaphore(const Device& device, std::uint64_t initial, const char* name = nullptr);
[[nodiscard]] UniqueFence createFence(const Device& device, bool signaled, const char* name = nullptr);

} // namespace lumora::rhi
