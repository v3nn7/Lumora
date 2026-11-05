#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <memory>
#include <vector>

namespace lumora::rhi {
class Device;
class BindlessTable;
}

namespace lumora::render {

struct PooledImage {
    rhi::Image image;
    std::uint32_t bindlessIndex = UINT32_MAX;
    VkImageUsageFlags usage = 0;
    std::uint32_t idleFrames = 0;
    bool inUse = false;
};

/// Recycles render-graph transient images. One pool per frame-in-flight slot: an image is
/// only reused after the slot's fence has signalled, so no cross-frame hazards exist.
class TransientImagePool {
public:
    static constexpr std::uint32_t kMaxIdleFrames = 8;

    TransientImagePool(const rhi::Device& device, rhi::BindlessTable& bindless);
    ~TransientImagePool();
    TransientImagePool(const TransientImagePool&) = delete;
    TransientImagePool& operator=(const TransientImagePool&) = delete;

    /// Start of a frame (after the slot fence wait): frees images idle for too long.
    void beginFrame();
    const PooledImage& acquire(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, const char* name);
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return images_.size(); }

private:
    const rhi::Device& device_;
    rhi::BindlessTable& bindless_;
    std::vector<std::unique_ptr<PooledImage>> images_;
};

} // namespace lumora::render
