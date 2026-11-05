#include "renderer/vulkan/BindlessTable.hpp"

#include "core/Log.hpp"
#include "renderer/vulkan/Device.hpp"

#include <algorithm>

namespace lumora::rhi {

BindlessTable::BindlessTable(const Device& device, std::uint32_t maxImages, std::uint32_t maxSamplers)
    : device_(device),
      maxImages_(std::min(maxImages, device.caps().maxBindlessSampledImages)),
      maxSamplers_(std::min(maxSamplers, device.caps().maxBindlessSamplers)) {
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const VkDescriptorBindingFlags bindingFlags[2] = {flags, flags};
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxImages_, VK_SHADER_STAGE_ALL, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, maxSamplers_, VK_SHADER_STAGE_ALL, nullptr},
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 2;
    flagsInfo.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateDescriptorSetLayout(device_.handle(), &layoutInfo, nullptr, &layout));
    layout_ = UniqueDescriptorSetLayout(device_.handle(), layout);

    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxImages_},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, maxSamplers_}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateDescriptorPool(device_.handle(), &poolInfo, nullptr, &pool));
    pool_ = UniqueDescriptorPool(device_.handle(), pool);

    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;
    LUMORA_VK_CHECK(vkAllocateDescriptorSets(device_.handle(), &alloc, &set_));
    device_.setName(VK_OBJECT_TYPE_DESCRIPTOR_SET, set_, "BindlessTable");
    log::info("Renderer", "bindless table: {} images, {} samplers", maxImages_, maxSamplers_);
}

BindlessTable::~BindlessTable() = default;

std::uint32_t BindlessTable::registerImage(VkImageView view, VkImageLayout layout) {
    std::uint32_t index = kInvalidIndex;
    {
        std::scoped_lock lock(mutex_);
        if (!freeImages_.empty()) {
            index = freeImages_.back();
            freeImages_.pop_back();
        } else if (nextImage_ < maxImages_) {
            index = nextImage_++;
        }
    }
    if (index == kInvalidIndex) {
        throw VulkanError("bindless image table is full", VK_ERROR_OUT_OF_POOL_MEMORY);
    }
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, view, layout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
    return index;
}

void BindlessTable::releaseImage(std::uint32_t index) {
    if (index == kInvalidIndex) return;
    std::scoped_lock lock(mutex_);
    freeImages_.push_back(index);
}

std::uint32_t BindlessTable::registerSampler(VkSampler sampler) {
    std::uint32_t index = 0;
    {
        std::scoped_lock lock(mutex_);
        if (nextSampler_ >= maxSamplers_) {
            throw VulkanError("bindless sampler table is full", VK_ERROR_OUT_OF_POOL_MEMORY);
        }
        index = nextSampler_++;
    }
    VkDescriptorImageInfo info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = 1;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
    return index;
}

std::uint32_t BindlessTable::liveImages() const {
    std::scoped_lock lock(mutex_);
    return nextImage_ - static_cast<std::uint32_t>(freeImages_.size());
}

} // namespace lumora::rhi
