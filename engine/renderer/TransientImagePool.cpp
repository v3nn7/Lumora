#include "renderer/TransientImagePool.hpp"

#include "renderer/vulkan/VulkanCommon.hpp"

namespace lumora::render {

TransientImagePool::TransientImagePool(nvrhi::IDevice* device, BindlessTable& bindless)
    : device_(device), bindless_(bindless) {}

TransientImagePool::~TransientImagePool() { clear(); }

void TransientImagePool::clear() {
    for (const auto& image : images_) bindless_.releaseTexture(image->bindlessIndex);
    images_.clear();
}

void TransientImagePool::beginFrame() {
    for (auto it = images_.begin(); it != images_.end();) {
        PooledImage& image = **it;
        if (!image.inUse && ++image.idleFrames > kMaxIdleFrames) {
            bindless_.releaseTexture(image.bindlessIndex);
            it = images_.erase(it);
            continue;
        }
        if (image.inUse) image.idleFrames = 0;
        image.inUse = false;
        ++it;
    }
}

const PooledImage& TransientImagePool::acquire(nvrhi::Format format, std::uint32_t width, std::uint32_t height,
                                               const char* name, bool storage) {
    for (const auto& image : images_) {
        const nvrhi::TextureDesc& d = image->texture->getDesc();
        if (!image->inUse && d.format == format && d.width == width && d.height == height && d.isUAV == storage) {
            image->inUse = true;
            return *image;
        }
    }
    const bool depth = nvrhi::getFormatInfo(format).hasDepth;
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.debugName = name;
    desc.isRenderTarget = true;
    desc.isUAV = storage;
    desc.setClearValue(depth ? nvrhi::Color(0.0f) : nvrhi::Color(0.0f));
    // Every command list starts and ends with transients sampleable; render passes move them
    // to attachment states and back.
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    auto image = std::make_unique<PooledImage>();
    image->texture = device_->createTexture(desc);
    if (!image->texture) throw rhi::VulkanError(std::string("could not create render target ") + name, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    image->bindlessIndex = bindless_.registerTexture(image->texture);
    image->inUse = true;
    images_.push_back(std::move(image));
    return *images_.back();
}

} // namespace lumora::render
