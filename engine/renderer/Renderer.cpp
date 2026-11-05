#include "renderer/Renderer.hpp"

#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "renderer/GpuTypes.hpp"
#include "renderer/RenderContext.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace lumora::render {

PresentTarget::PresentTarget(RenderContext& context, VkSurfaceKHR surface, VkExtent2D extent, rhi::PresentPolicy policy)
    : swapchain_(std::make_unique<rhi::Swapchain>(context.device(), surface, extent, policy)), desired_(extent) {}

PresentTarget::~PresentTarget() = default;

void PresentTarget::resize(VkExtent2D extent) noexcept {
    if (extent.width != desired_.width || extent.height != desired_.height) {
        desired_ = extent;
        dirty_ = true;
    }
}

void PresentTarget::setPolicy(rhi::PresentPolicy policy) noexcept {
    if (policy != swapchain_->policy()) {
        swapchain_->setPolicy(policy);
        dirty_ = true;
    }
}

Renderer::Renderer(RenderContext& context)
    : ctx_(context), sceneRenderer_(context), start_(std::chrono::steady_clock::now()) {
    rhi::Device& device = ctx_.device();

    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateDescriptorPool(device.handle(), &poolInfo, nullptr, &pool));
    descriptorPool_ = rhi::UniqueDescriptorPool(device.handle(), pool);

    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        FrameResources& f = frames_[i];
        VkCommandPoolCreateInfo cpInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cpInfo.queueFamilyIndex = device.graphicsFamily();
        VkCommandPool cp = VK_NULL_HANDLE;
        LUMORA_VK_CHECK(vkCreateCommandPool(device.handle(), &cpInfo, nullptr, &cp));
        f.commandPool = rhi::UniqueCommandPool(device.handle(), cp);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = cp;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        LUMORA_VK_CHECK(vkAllocateCommandBuffers(device.handle(), &alloc, &f.cmd));
        const std::string name = std::format("Frame{}", i);
        device.setName(VK_OBJECT_TYPE_COMMAND_BUFFER, f.cmd, name.c_str());

        f.fence = rhi::createFence(device, false, "FrameFence");
        f.acquired = rhi::createBinarySemaphore(device, "ImageAcquired");
        f.constants = rhi::Buffer(device, {sizeof(FrameConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           rhi::MemoryUsage::CpuToGpu, "FrameConstants"});

        const VkDescriptorSetLayout layout = ctx_.frameSetLayout();
        VkDescriptorSetAllocateInfo setAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAlloc.descriptorPool = pool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &layout;
        LUMORA_VK_CHECK(vkAllocateDescriptorSets(device.handle(), &setAlloc, &f.set));
        const VkDescriptorBufferInfo ubo{f.constants.handle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = f.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &ubo;
        vkUpdateDescriptorSets(device.handle(), 1, &write, 0, nullptr);

        f.transients = std::make_unique<TransientImagePool>(device, ctx_.bindless());
        f.profiler = std::make_unique<GpuProfiler>(device);
    }
}

Renderer::~Renderer() {
    try {
        waitIdle();
    } catch (const std::exception& e) {
        log::error("Renderer", "shutdown wait failed: {}", e.what());
    }
}

void Renderer::waitIdle() {
    std::vector<VkFence> fences;
    for (auto& f : frames_) {
        if (f.pending) fences.push_back(f.fence.get());
    }
    if (!fences.empty()) {
        LUMORA_VK_CHECK(vkWaitForFences(ctx_.device().handle(), static_cast<std::uint32_t>(fences.size()),
                                        fences.data(), VK_TRUE, UINT64_MAX));
        for (auto& f : frames_) f.pending = false;
    }
}

FrameResources& Renderer::beginFrame() {
    FrameResources& frame = frames_[slot_];
    VkDevice device = ctx_.device().handle();
    if (frame.pending) {
        const VkFence fence = frame.fence.get();
        LUMORA_VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        frame.pending = false;
        frame.profiler->resolve();
        stats_.gpuFrameMs = frame.profiler->frameMilliseconds();
        stats_.gpuPasses = frame.profiler->results();
    }
    frame.transients->beginFrame();
    ctx_.uploads().collect();
    return frame;
}

void Renderer::advance() noexcept { slot_ = (slot_ + 1) % kFramesInFlight; }

SceneDrawStats Renderer::record(FrameResources& frame, const Scene& scene, const CameraView& camera,
                                const RenderSettings& settings, const ImportedImage& output,
                                const std::function<void(RenderGraph&, RGImage)>& extraPasses) {
    VkDevice device = ctx_.device().handle();
    LUMORA_VK_CHECK(vkResetCommandPool(device, frame.commandPool.get(), 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    LUMORA_VK_CHECK(vkBeginCommandBuffer(frame.cmd, &begin));
    frame.profiler->beginFrame(frame.cmd);

    // Re-derive the projection from the real target size so resizes never stretch the image.
    CameraView view = camera;
    view.aspect = static_cast<float>(output.extent.width) / static_cast<float>(std::max(output.extent.height, 1u));
    view.projection = perspectiveReverseZ(view.fovYRadians, view.aspect, view.nearPlane);

    RenderGraph graph;
    const RGImage out = graph.importImage("Output", output);
    const double time = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    const SceneDrawStats drawStats =
        sceneRenderer_.build(graph, frame, scene, view, settings, out, output.format, output.extent, time);
    if (extraPasses) extraPasses(graph, out);
    graph.execute(frame.cmd, ctx_.device(), *frame.transients, frame.profiler.get());

    frame.profiler->endFrame(frame.cmd);
    LUMORA_VK_CHECK(vkEndCommandBuffer(frame.cmd));
    return drawStats;
}

void Renderer::submit(FrameResources& frame, VkSemaphore waitAcquired, VkSemaphore signalPresent) {
    std::vector<VkSemaphoreSubmitInfo> waits;
    if (waitAcquired != VK_NULL_HANDLE) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        w.semaphore = waitAcquired;
        w.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        waits.push_back(w);
    }
    if (const std::uint64_t uploadValue = ctx_.uploads().lastSubmittedValue(); uploadValue > 0) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        w.semaphore = ctx_.uploads().timeline();
        w.value = uploadValue;
        w.stageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        waits.push_back(w);
    }
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = signalPresent;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = frame.cmd;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
    info.pWaitSemaphoreInfos = waits.data();
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &cmdInfo;
    info.signalSemaphoreInfoCount = signalPresent != VK_NULL_HANDLE ? 1u : 0u;
    info.pSignalSemaphoreInfos = &signal;
    const VkFence fence = frame.fence.get();
    LUMORA_VK_CHECK(vkResetFences(ctx_.device().handle(), 1, &fence));
    ctx_.device().submit(info, fence);
    frame.pending = true;
}

FrameResult Renderer::render(PresentTarget& target, const Scene& scene, const CameraView& camera,
                             const RenderSettings& settings) {
    rhi::Swapchain& swapchain = *target.swapchain_;
    if (target.dirty_ || !swapchain.valid()) {
        waitIdle();
        if (!swapchain.recreate(target.desired_)) {
            return FrameResult::Skipped;  // zero-sized (minimized) or surface temporarily unavailable
        }
        target.dirty_ = false;
    }

    FrameResources& frame = beginFrame();
    std::uint32_t imageIndex = 0;
    const rhi::SwapchainStatus acquired = swapchain.acquire(frame.acquired.get(), imageIndex);
    if (acquired == rhi::SwapchainStatus::OutOfDate) {
        target.dirty_ = true;
        return FrameResult::Skipped;
    }
    if (acquired == rhi::SwapchainStatus::SurfaceLost) {
        return FrameResult::SurfaceLost;
    }
    if (acquired == rhi::SwapchainStatus::Timeout) {
        return FrameResult::Skipped;  // no image available yet; the acquire semaphore was not signalled
    }
    if (acquired == rhi::SwapchainStatus::Suboptimal) {
        target.dirty_ = true;  // still render this image, rebuild next frame
    }

    Stopwatch cpu;
    ImportedImage output;
    output.image = swapchain.image(imageIndex);
    output.view = swapchain.view(imageIndex);
    output.format = swapchain.format();
    output.extent = swapchain.extent();
    // Chains with the acquire semaphore wait at COLOR_ATTACHMENT_OUTPUT.
    output.initialState = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE};
    output.finalUsage = ImageUsage::Present;
    stats_.scene = record(frame, scene, camera, settings, output, {});
    submit(frame, frame.acquired.get(), swapchain.presentSemaphore(imageIndex));
    stats_.cpuRecordMs = cpu.elapsedMs();
    stats_.extent = output.extent;
    stats_.frameNumber = ++frameNumber_;
    stats_.vram = ctx_.device().deviceLocalMemoryUsage();

    const rhi::SwapchainStatus presented = swapchain.present(imageIndex);
    advance();
    if (presented == rhi::SwapchainStatus::OutOfDate || presented == rhi::SwapchainStatus::Suboptimal) {
        target.dirty_ = true;
    } else if (presented == rhi::SwapchainStatus::SurfaceLost) {
        return FrameResult::SurfaceLost;
    }
    return FrameResult::Rendered;
}

Result<CapturedImage> Renderer::capture(const Scene& scene, const CameraView& camera, VkExtent2D extent,
                                        const RenderSettings& settings) {
    if (extent.width == 0 || extent.height == 0 || extent.width > 16384 || extent.height > 16384) {
        return makeError("invalid capture size {}x{}", extent.width, extent.height);
    }
    rhi::Device& device = ctx_.device();
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_SRGB;
    rhi::Image target(device, {kFormat, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               1, "CaptureTarget"});
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    rhi::Buffer readback(device, {bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuToCpu, "CaptureReadback"});

    FrameResources& frame = beginFrame();
    ImportedImage output;
    output.image = target.handle();
    output.view = target.view();
    output.format = kFormat;
    output.extent = extent;
    const VkBuffer dst = readback.handle();
    stats_.scene = record(frame, scene, camera, settings, output, [&](RenderGraph& graph, RGImage out) {
        graph.addPass(
            "Readback", [&](PassBuilder& b) { b.copyFrom(out); },
            [dst, extent, out](PassContext& ctx) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {extent.width, extent.height, 1};
                vkCmdCopyImageToBuffer(ctx.cmd, ctx.image(out), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
                // Make the copy visible to host reads after the fence wait.
                VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = dst;
                barrier.size = VK_WHOLE_SIZE;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(ctx.cmd, &dep);
            });
    });
    submit(frame, VK_NULL_HANDLE, VK_NULL_HANDLE);
    const VkFence fence = frame.fence.get();
    LUMORA_VK_CHECK(vkWaitForFences(device.handle(), 1, &fence, VK_TRUE, UINT64_MAX));
    frame.pending = false;
    frame.profiler->resolve();
    stats_.gpuFrameMs = frame.profiler->frameMilliseconds();
    stats_.gpuPasses = frame.profiler->results();
    advance();

    readback.invalidate();
    CapturedImage image;
    image.width = extent.width;
    image.height = extent.height;
    image.rgba.resize(static_cast<std::size_t>(bytes));
    std::memcpy(image.rgba.data(), readback.mapped(), image.rgba.size());
    return image;
}

} // namespace lumora::render
