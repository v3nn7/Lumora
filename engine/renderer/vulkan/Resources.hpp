#pragma once

#include "renderer/vulkan/VulkanCommon.hpp"

#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace lumora::rhi {

class Device;

enum class MemoryUsage {
    GpuOnly,     // device-local, not mappable
    CpuToGpu,    // host-visible, persistently mapped, sequential writes (uniforms, staging)
    GpuToCpu,    // host-visible, cached, persistently mapped (readback)
};

struct BufferDesc {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    MemoryUsage memory = MemoryUsage::GpuOnly;
    const char* debugName = nullptr;
};

/// RAII VkBuffer + VMA allocation. Movable, non-copyable.
class Buffer {
public:
    Buffer() = default;
    Buffer(const Device& device, const BufferDesc& desc);
    ~Buffer();
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] VkBuffer handle() const noexcept { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
    [[nodiscard]] void* mapped() const noexcept { return mapped_; }
    [[nodiscard]] explicit operator bool() const noexcept { return buffer_ != VK_NULL_HANDLE; }

    /// Copies into a host-visible buffer and flushes if the memory is non-coherent.
    void write(std::span<const std::byte> data, VkDeviceSize offset = 0);
    /// Makes GPU writes visible to the host for GpuToCpu buffers.
    void invalidate();

private:
    void release() noexcept;

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

struct ImageDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{1, 1};
    VkImageUsageFlags usage = 0;
    std::uint32_t mipLevels = 1;
    const char* debugName = nullptr;
};

/// RAII 2D VkImage + default VkImageView + VMA allocation.
class Image {
public:
    Image() = default;
    Image(const Device& device, const ImageDesc& desc);
    ~Image();
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    [[nodiscard]] VkImage handle() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkImageAspectFlags aspect() const noexcept { return aspect_; }
    [[nodiscard]] explicit operator bool() const noexcept { return image_ != VK_NULL_HANDLE; }

private:
    void release() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_COLOR_BIT;
};

[[nodiscard]] bool isDepthFormat(VkFormat format) noexcept;
[[nodiscard]] bool isSrgbFormat(VkFormat format) noexcept;

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
