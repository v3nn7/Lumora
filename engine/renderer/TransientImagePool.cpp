#include "renderer/TransientImagePool.hpp"

#include "renderer/vulkan/BindlessTable.hpp"
#include "renderer/vulkan/Device.hpp"

#include <algorithm>

namespace lumora::render {

TransientImagePool::TransientImagePool(const rhi::Device& device, rhi::BindlessTable& bindless)
    : device_(device), bindless_(bindless) {}

TransientImagePool::~TransientImagePool() { clear(); }

void TransientImagePool::clear() {
    for (auto& img : images_) bindless_.releaseImage(img->bindlessIndex);
    images_.clear();
}

void TransientImagePool::beginFrame() {
    for (auto& img : images_) {
        img->idleFrames = img->inUse ? 0 : img->idleFrames + 1;
        img->inUse = false;
    }
    std::erase_if(images_, [&](const std::unique_ptr<PooledImage>& img) {
        if (img->idleFrames <= kMaxIdleFrames) return false;
        bindless_.releaseImage(img->bindlessIndex);
        return true;
    });
}

const PooledImage& TransientImagePool::acquire(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                               const char* name) {
    for (auto& img : images_) {
        const VkExtent2D e = img->image.extent();
        if (!img->inUse && img->image.format() == format && e.width == extent.width && e.height == extent.height &&
            img->usage == usage) {
            img->inUse = true;
            return *img;
        }
    }
    auto img = std::make_unique<PooledImage>();
    img->image = rhi::Image(device_, {format, extent, usage, 1, name});
    img->usage = usage;
    img->inUse = true;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        img->bindlessIndex = bindless_.registerImage(img->image.view());
    }
    images_.push_back(std::move(img));
    return *images_.back();
}

} // namespace lumora::render
