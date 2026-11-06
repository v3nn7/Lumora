#pragma once

#include "renderer/Gpu.hpp"

#include <memory>
#include <vector>

namespace lumora::render {

class BindlessTable;

struct PooledImage {
    nvrhi::TextureHandle texture;
    std::uint32_t bindlessIndex = UINT32_MAX;
    std::uint32_t idleFrames = 0;
    bool inUse = false;
};

/// Recycles render-graph transient textures (render targets, depth). One pool per
/// frame-in-flight slot: a texture is only reused after that slot's frame finished on the GPU.
/// Every texture is registered in the bindless table once, so passes can sample it.
class TransientImagePool {
public:
    static constexpr std::uint32_t kMaxIdleFrames = 8;

    TransientImagePool(nvrhi::IDevice* device, BindlessTable& bindless);
    ~TransientImagePool();
    TransientImagePool(const TransientImagePool&) = delete;
    TransientImagePool& operator=(const TransientImagePool&) = delete;

    /// Start of a frame (after the slot's frame completed): frees textures idle for too long.
    void beginFrame();
    const PooledImage& acquire(nvrhi::Format format, std::uint32_t width, std::uint32_t height, const char* name,
                               bool storage = false);
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return images_.size(); }

private:
    nvrhi::IDevice* device_;
    BindlessTable& bindless_;
    std::vector<std::unique_ptr<PooledImage>> images_;
};

} // namespace lumora::render
