#include "renderer/vulkan/UploadQueue.hpp"

#include "renderer/vulkan/Device.hpp"

#include <algorithm>

namespace lumora::rhi {

UploadQueue::UploadQueue(Device& device) : device_(device) {
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = device_.graphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateCommandPool(device_.handle(), &info, nullptr, &pool));
    pool_ = UniqueCommandPool(device_.handle(), pool);
    timeline_ = createTimelineSemaphore(device_, 0, "UploadTimeline");
}

UploadQueue::~UploadQueue() {
    try {
        waitIdle();
    } catch (...) {
        // Device lost during shutdown: nothing left to wait for.
    }
    for (auto& p : pending_) {
        vkFreeCommandBuffers(device_.handle(), pool_.get(), 1, &p.cmd);
    }
}

VkCommandBuffer UploadQueue::beginCommands() {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_.get();
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkAllocateCommandBuffers(device_.handle(), &alloc, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    LUMORA_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
}

void UploadQueue::submit(VkCommandBuffer cmd, Buffer staging) {
    LUMORA_VK_CHECK(vkEndCommandBuffer(cmd));
    const std::uint64_t value = ++lastSubmitted_;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = timeline_.get();
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;  // copies, blits and final layout transitions
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signal;
    device_.submit(submitInfo, VK_NULL_HANDLE);
    pending_.push_back({value, std::move(staging), cmd});
}

Buffer UploadQueue::uploadBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage, const char* name) {
    std::scoped_lock lock(mutex_);
    Buffer target(device_, {data.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, name});
    Buffer staging(device_, {data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu, "UploadStaging"});
    staging.write(data);
    VkCommandBuffer cmd = beginCommands();
    const VkBufferCopy region{0, 0, data.size()};
    vkCmdCopyBuffer(cmd, staging.handle(), target.handle(), 1, &region);
    submit(cmd, std::move(staging));
    return target;
}

Image UploadQueue::uploadImage(std::span<const std::byte> rgba, VkExtent2D extent, VkFormat format, bool mipmaps,
                               const char* name) {
    std::scoped_lock lock(mutex_);
    if (rgba.size() != static_cast<std::size_t>(extent.width) * extent.height * 4) {
        throw VulkanError("uploadImage: pixel data does not match the extent", VK_ERROR_UNKNOWN);
    }
    const bool canBlit = device_.supportsFormat(format, VK_FORMAT_FEATURE_2_BLIT_SRC_BIT | VK_FORMAT_FEATURE_2_BLIT_DST_BIT |
                                                            VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    std::uint32_t levels = 1;
    if (mipmaps && canBlit) {
        for (std::uint32_t size = std::max(extent.width, extent.height); size > 1; size /= 2) ++levels;
    }
    Image image(device_, {format, extent,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          levels, name});
    Buffer staging(device_, {rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu, "UploadStaging"});
    staging.write(rgba);
    VkCommandBuffer cmd = beginCommands();

    auto barrier = [&](std::uint32_t mip, std::uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.handle();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    barrier(0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    std::int32_t w = static_cast<std::int32_t>(extent.width);
    std::int32_t h = static_cast<std::int32_t>(extent.height);
    for (std::uint32_t mip = 1; mip < levels; ++mip) {
        barrier(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        const std::int32_t nw = std::max(w / 2, 1);
        const std::int32_t nh = std::max(h / 2, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        w = nw;
        h = nh;
    }
    barrier(levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    submit(cmd, std::move(staging));
    return image;
}

Image UploadQueue::uploadImageLevels(std::span<const std::span<const std::byte>> levels,
                                     std::span<const VkExtent2D> extents, VkFormat format, std::uint32_t texelBytes,
                                     const char* name) {
    std::scoped_lock lock(mutex_);
    if (levels.empty() || levels.size() != extents.size()) {
        throw VulkanError("uploadImageLevels: one extent per level is required", VK_ERROR_UNKNOWN);
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[i].size() != static_cast<std::size_t>(extents[i].width) * extents[i].height * texelBytes) {
            throw VulkanError("uploadImageLevels: level data does not match its extent", VK_ERROR_UNKNOWN);
        }
        // Mip i of a Vulkan image is max(1, base >> i) in each dimension.
        if (extents[i].width != std::max(extents[0].width >> i, 1u) || extents[i].height != std::max(extents[0].height >> i, 1u)) {
            throw VulkanError("uploadImageLevels: level extents do not form a mip chain", VK_ERROR_UNKNOWN);
        }
        total += (levels[i].size() + 15) & ~std::size_t{15};  // keep every level 16-byte aligned
    }
    const auto count = static_cast<std::uint32_t>(levels.size());
    Image image(device_, {format, extents[0], VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, count, name});
    Buffer staging(device_, {total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu, "UploadStaging"});
    std::vector<std::byte> packed(total);
    std::vector<VkBufferImageCopy> copies(count);
    std::size_t offset = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::copy(levels[i].begin(), levels[i].end(), packed.begin() + static_cast<std::ptrdiff_t>(offset));
        copies[i].bufferOffset = offset;
        copies[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        copies[i].imageExtent = {extents[i].width, extents[i].height, 1};
        offset += (levels[i].size() + 15) & ~std::size_t{15};
    }
    staging.write(packed);
    VkCommandBuffer cmd = beginCommands();
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.handle();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, count, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, staging.handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count,
                           copies.data());
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    submit(cmd, std::move(staging));
    return image;
}

void UploadQueue::collect() {
    std::scoped_lock lock(mutex_);
    std::uint64_t completed = 0;
    LUMORA_VK_CHECK(vkGetSemaphoreCounterValue(device_.handle(), timeline_.get(), &completed));
    while (!pending_.empty() && pending_.front().value <= completed) {
        vkFreeCommandBuffers(device_.handle(), pool_.get(), 1, &pending_.front().cmd);
        pending_.pop_front();
    }
}

void UploadQueue::waitIdle() {
    if (lastSubmitted_ == 0) return;
    VkSemaphore sem = timeline_.get();
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &sem;
    wait.pValues = &lastSubmitted_;
    LUMORA_VK_CHECK(vkWaitSemaphores(device_.handle(), &wait, UINT64_MAX));
    collect();
}

} // namespace lumora::rhi
