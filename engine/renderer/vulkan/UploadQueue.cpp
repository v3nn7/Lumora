#include "renderer/vulkan/UploadQueue.hpp"

#include "renderer/vulkan/Device.hpp"

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

Buffer UploadQueue::uploadBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage, const char* name) {
    std::scoped_lock lock(mutex_);
    Buffer target(device_, {data.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, name});
    Buffer staging(device_, {data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu, "UploadStaging"});
    staging.write(data);

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_.get();
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkAllocateCommandBuffers(device_.handle(), &alloc, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    LUMORA_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    const VkBufferCopy region{0, 0, data.size()};
    vkCmdCopyBuffer(cmd, staging.handle(), target.handle(), 1, &region);
    LUMORA_VK_CHECK(vkEndCommandBuffer(cmd));

    const std::uint64_t value = ++lastSubmitted_;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = timeline_.get();
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;  // signal after the copy completes
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    device_.submit(submit, VK_NULL_HANDLE);

    pending_.push_back({value, std::move(staging), cmd});
    return target;
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
