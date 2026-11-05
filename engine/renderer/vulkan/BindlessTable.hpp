#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace lumora::rhi {

class Device;

/// Global descriptor set with unbounded arrays of sampled images and samplers
/// (descriptor indexing, update-after-bind). Shaders index them with handles passed in
/// push constants or buffers: set = 1, binding 0 = Texture2D[], binding 1 = SamplerState[].
class BindlessTable {
public:
    static constexpr std::uint32_t kSet = 1;
    static constexpr std::uint32_t kInvalidIndex = UINT32_MAX;

    explicit BindlessTable(const Device& device, std::uint32_t maxImages = 4096, std::uint32_t maxSamplers = 32);
    ~BindlessTable();
    BindlessTable(const BindlessTable&) = delete;
    BindlessTable& operator=(const BindlessTable&) = delete;

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return layout_.get(); }
    [[nodiscard]] VkDescriptorSet set() const noexcept { return set_; }

    [[nodiscard]] std::uint32_t registerImage(VkImageView view,
                                              VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    void releaseImage(std::uint32_t index);
    [[nodiscard]] std::uint32_t registerSampler(VkSampler sampler);
    [[nodiscard]] std::uint32_t imageCapacity() const noexcept { return maxImages_; }
    [[nodiscard]] std::uint32_t liveImages() const;

private:
    const Device& device_;
    std::uint32_t maxImages_;
    std::uint32_t maxSamplers_;
    UniqueDescriptorSetLayout layout_;
    UniqueDescriptorPool pool_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    mutable std::mutex mutex_;
    std::vector<std::uint32_t> freeImages_;
    std::uint32_t nextImage_ = 0;
    std::uint32_t nextSampler_ = 0;
};

} // namespace lumora::rhi
