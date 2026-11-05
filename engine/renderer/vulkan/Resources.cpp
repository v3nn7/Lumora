#include "renderer/vulkan/Resources.hpp"

#include "renderer/vulkan/Device.hpp"

#include <cstring>

namespace lumora::rhi {

Buffer::Buffer(const Device& device, const BufferDesc& desc) : allocator_(device.allocator()), size_(desc.size) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = desc.size;
    info.usage = desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    switch (desc.memory) {
        case MemoryUsage::GpuOnly: alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
        case MemoryUsage::CpuToGpu:
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUsage::GpuToCpu:
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }
    VmaAllocationInfo result{};
    LUMORA_VK_CHECK(vmaCreateBuffer(allocator_, &info, &alloc, &buffer_, &allocation_, &result));
    mapped_ = result.pMappedData;
    if (desc.debugName != nullptr) {
        device.setName(VK_OBJECT_TYPE_BUFFER, buffer_, desc.debugName);
        vmaSetAllocationName(allocator_, allocation_, desc.debugName);
    }
}

Buffer::~Buffer() { release(); }

Buffer::Buffer(Buffer&& o) noexcept
    : allocator_(std::exchange(o.allocator_, VK_NULL_HANDLE)),
      buffer_(std::exchange(o.buffer_, VK_NULL_HANDLE)),
      allocation_(std::exchange(o.allocation_, VK_NULL_HANDLE)),
      size_(std::exchange(o.size_, 0)),
      mapped_(std::exchange(o.mapped_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& o) noexcept {
    if (this != &o) {
        release();
        allocator_ = std::exchange(o.allocator_, VK_NULL_HANDLE);
        buffer_ = std::exchange(o.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(o.allocation_, VK_NULL_HANDLE);
        size_ = std::exchange(o.size_, 0);
        mapped_ = std::exchange(o.mapped_, nullptr);
    }
    return *this;
}

void Buffer::release() noexcept {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    mapped_ = nullptr;
}

void Buffer::write(std::span<const std::byte> data, VkDeviceSize offset) {
    if (mapped_ == nullptr) {
        throw VulkanError("Buffer::write on a buffer that is not host-mapped", VK_ERROR_MEMORY_MAP_FAILED);
    }
    if (offset + data.size() > size_) {
        throw VulkanError("Buffer::write out of range", VK_ERROR_UNKNOWN);
    }
    std::memcpy(static_cast<std::byte*>(mapped_) + offset, data.data(), data.size());
    LUMORA_VK_CHECK(vmaFlushAllocation(allocator_, allocation_, offset, data.size()));
}

void Buffer::invalidate() {
    if (allocation_ != VK_NULL_HANDLE) {
        LUMORA_VK_CHECK(vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE));
    }
}

bool isDepthFormat(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            return true;
        default:
            return false;
    }
}

bool isSrgbFormat(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
    }
}

Image::Image(const Device& device, const ImageDesc& desc)
    : device_(device.handle()), allocator_(device.allocator()), format_(desc.format), extent_(desc.extent) {
    aspect_ = isDepthFormat(desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = desc.format;
    info.extent = {desc.extent.width, desc.extent.height, 1};
    info.mipLevels = desc.mipLevels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // Render targets benefit from dedicated allocations on most drivers.
    if (desc.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        alloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    LUMORA_VK_CHECK(vmaCreateImage(allocator_, &info, &alloc, &image_, &allocation_, nullptr));

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = desc.format;
    viewInfo.subresourceRange = {aspect_, 0, desc.mipLevels, 0, 1};
    LUMORA_VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &view_));

    if (desc.debugName != nullptr) {
        device.setName(VK_OBJECT_TYPE_IMAGE, image_, desc.debugName);
        device.setName(VK_OBJECT_TYPE_IMAGE_VIEW, view_, desc.debugName);
        vmaSetAllocationName(allocator_, allocation_, desc.debugName);
    }
}

Image::~Image() { release(); }

Image::Image(Image&& o) noexcept
    : device_(std::exchange(o.device_, VK_NULL_HANDLE)),
      allocator_(std::exchange(o.allocator_, VK_NULL_HANDLE)),
      image_(std::exchange(o.image_, VK_NULL_HANDLE)),
      view_(std::exchange(o.view_, VK_NULL_HANDLE)),
      allocation_(std::exchange(o.allocation_, VK_NULL_HANDLE)),
      format_(o.format_),
      extent_(o.extent_),
      aspect_(o.aspect_) {}

Image& Image::operator=(Image&& o) noexcept {
    if (this != &o) {
        release();
        device_ = std::exchange(o.device_, VK_NULL_HANDLE);
        allocator_ = std::exchange(o.allocator_, VK_NULL_HANDLE);
        image_ = std::exchange(o.image_, VK_NULL_HANDLE);
        view_ = std::exchange(o.view_, VK_NULL_HANDLE);
        allocation_ = std::exchange(o.allocation_, VK_NULL_HANDLE);
        format_ = o.format_;
        extent_ = o.extent_;
        aspect_ = o.aspect_;
    }
    return *this;
}

void Image::release() noexcept {
    if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, view_, nullptr);
    if (image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, image_, allocation_);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
}

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
