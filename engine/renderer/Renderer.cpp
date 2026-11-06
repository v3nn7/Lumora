#include "renderer/Renderer.hpp"

#include "core/JobSystem.hpp"
#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "renderer/GpuTypes.hpp"
#include "renderer/RenderContext.hpp"
#include "scene/Scene.hpp"

#include <nvrhi/vulkan.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace lumora::render {

namespace {

/// More threads than this rarely pay off for one pass.
constexpr std::uint32_t kMaxRecordingThreads = 8;

nvrhi::Format fromVulkan(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB: return nvrhi::Format::SBGRA8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return nvrhi::Format::BGRA8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return nvrhi::Format::SRGBA8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: return nvrhi::Format::RGBA8_UNORM;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return nvrhi::Format::R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return nvrhi::Format::RGBA16_FLOAT;
        default: return nvrhi::Format::UNKNOWN;
    }
}

} // namespace

PresentTarget::PresentTarget(RenderContext& context, VkSurfaceKHR surface, VkExtent2D extent, rhi::PresentPolicy policy)
    : ctx_(context), swapchain_(std::make_unique<rhi::Swapchain>(context.device(), surface, extent, policy)),
      desired_(extent) {}

PresentTarget::~PresentTarget() {
    // NVRHI's views of the swapchain images go before the swapchain does.
    ctx_.gpu()->waitForIdle();
    releaseImages();
}

void PresentTarget::releaseImages() {
    images_.clear();
    ctx_.gpu()->runGarbageCollection();
}

nvrhi::ITexture* PresentTarget::image(std::uint32_t index) {
    if (images_.size() != swapchain_->imageCount()) images_.assign(swapchain_->imageCount(), nullptr);
    nvrhi::TextureHandle& texture = images_[index];
    if (!texture) {
        nvrhi::TextureDesc desc;
        desc.width = swapchain_->extent().width;
        desc.height = swapchain_->extent().height;
        desc.format = fromVulkan(swapchain_->format());
        desc.debugName = std::format("Swapchain{}", index);
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::Present;
        desc.keepInitialState = true;
        texture = ctx_.gpu()->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
                                                           nvrhi::Object(swapchain_->image(index)), desc);
    }
    return texture;
}

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
    nvrhi::IDevice* gpu = ctx_.gpu();
    for (FrameResources& f : frames_) {
        f.done = gpu->createEventQuery();
        f.acquired = rhi::createBinarySemaphore(ctx_.device(), "ImageAcquired");
        f.transients = std::make_unique<TransientImagePool>(gpu, ctx_.bindless());
        f.profiler = std::make_unique<GpuProfiler>(gpu, ctx_.caps().timestamps);
    }
}

Renderer::~Renderer() {
    try {
        waitIdle();
    } catch (const std::exception& e) {
        log::error("Renderer", "shutdown wait failed: {}", e.what());
    }
    framebuffers_.clear();
    for (FrameResources& f : frames_) {
        f.transients.reset();
        f.lists.clear();
    }
    ctx_.gpu()->runGarbageCollection();
}

void Renderer::invalidateAssetFile(const std::filesystem::path& absoluteFile) {
    waitIdle();
    const std::string file = absoluteFile.lexically_normal().generic_string();
    const std::size_t meshes = ctx_.meshes().invalidateFile(file);
    const std::size_t textures = ctx_.textures().invalidateFile(file);
    ctx_.gpu()->runGarbageCollection();
    if (meshes + textures > 0) log::info("Renderer", "reloading {} ({} meshes, {} textures)", file, meshes, textures);
}

void Renderer::waitIdle() {
    for (FrameResources& f : frames_) {
        if (f.pending) finish(f);
    }
    ctx_.gpu()->waitForIdle();
}

void Renderer::finish(FrameResources& frame) {
    ctx_.gpu()->waitEventQuery(frame.done);
    frame.pending = false;
    frame.profiler->resolve();
    stats_.gpuFrameMs = frame.profiler->frameMilliseconds();
    stats_.gpuPasses = frame.profiler->results();
}

FrameResources& Renderer::beginFrame() {
    FrameResources& frame = frames_[slot_];
    if (frame.pending) finish(frame);
    frame.usedLists = 0;
    frame.transients->beginFrame();
    framebuffers_.prune(frameNumber_);
    ctx_.gpu()->runGarbageCollection();
    return frame;
}

void Renderer::advance() noexcept { slot_ = (slot_ + 1) % kFramesInFlight; }

std::vector<nvrhi::CommandListHandle> Renderer::record(FrameResources& frame, const Scene& scene, const CameraView& camera,
                                                       const RenderSettings& settings, const ImportedImage& output,
                                                       const std::function<void(RenderGraph&, RGImage)>& extraPasses) {
    nvrhi::IDevice* gpu = ctx_.gpu();
    nvrhi::CommandListHandle first = frame.nextList(gpu);
    frame.profiler->beginFrame(first);
    const nvrhi::TextureDesc& outDesc = output.texture->getDesc();
    // Re-derive the projection from the real target size so resizes never stretch the image.
    CameraView view = camera;
    view.aspect = static_cast<float>(outDesc.width) / static_cast<float>(std::max(outDesc.height, 1u));
    view.projection = perspectiveReverseZ(view.fovYRadians, view.aspect, view.nearPlane);

    RenderGraph graph;
    const RGImage out = graph.importImage("Output", output);
    const double time = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    stats_.scene = sceneRenderer_.build(graph, frame, scene, view, settings, out, outDesc.format,
                                        {outDesc.width, outDesc.height}, time);
    if (extraPasses) extraPasses(graph, out);
    GraphRuntime runtime;
    runtime.device = gpu;
    runtime.pool = frame.transients.get();
    runtime.profiler = frame.profiler.get();
    runtime.framebuffers = &framebuffers_;
    runtime.frameBindings = frame.bindingSet(gpu, ctx_.frameLayout(), ctx_.samplers());
    runtime.storageLayout = ctx_.storageLayout();
    runtime.storageFallback = ctx_.storageFallback();
    runtime.frame = frameNumber_;
    runtime.newList = [&frame, gpu] { return frame.nextList(gpu); };
    runtime.maxThreads = std::min(JobSystem::global().workerCount() + 1, kMaxRecordingThreads);
    std::vector<nvrhi::CommandListHandle> lists = graph.execute(first, runtime);
    frame.profiler->endFrame(lists.back());
    lists.back()->close();
    return lists;
}

void Renderer::submit(FrameResources& frame, std::vector<nvrhi::CommandListHandle>& lists) {
    std::vector<nvrhi::ICommandList*> raw;
    raw.reserve(lists.size());
    for (const auto& l : lists) raw.push_back(l);
    ctx_.gpu()->executeCommandLists(raw.data(), raw.size());
    ctx_.gpu()->resetEventQuery(frame.done);
    ctx_.gpu()->setEventQuery(frame.done, nvrhi::CommandQueue::Graphics);
    frame.pending = true;
}

FrameResult Renderer::render(PresentTarget& target, const Scene& scene, const CameraView& camera,
                             const RenderSettings& settings) {
    rhi::Swapchain& swapchain = *target.swapchain_;
    if (target.dirty_ || !swapchain.valid()) {
        waitIdle();
        framebuffers_.clear();
        target.releaseImages();
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
    if (acquired == rhi::SwapchainStatus::SurfaceLost) return FrameResult::SurfaceLost;
    if (acquired == rhi::SwapchainStatus::Timeout) return FrameResult::Skipped;  // the semaphore was not signalled
    if (acquired == rhi::SwapchainStatus::Suboptimal) target.dirty_ = true;  // still render this image

    Stopwatch cpu;
    ImportedImage output;
    output.texture = target.image(imageIndex);
    std::vector<nvrhi::CommandListHandle> lists = record(frame, scene, camera, settings, output, {});
    ctx_.vulkanGpu()->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, frame.acquired.get(), 0);
    ctx_.vulkanGpu()->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, swapchain.presentSemaphore(imageIndex), 0);
    submit(frame, lists);
    stats_.cpuRecordMs = cpu.elapsedMs();
    stats_.extent = swapchain.extent();
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
                                        const RenderSettings& settings,
                                        const std::function<void(RenderGraph&, RGImage)>& extraPasses) {
    if (extent.width == 0 || extent.height == 0 || extent.width > 16384 || extent.height > 16384) {
        return makeError("invalid capture size {}x{}", extent.width, extent.height);
    }
    nvrhi::IDevice* gpu = ctx_.gpu();
    nvrhi::TextureDesc desc;
    desc.width = extent.width;
    desc.height = extent.height;
    desc.format = nvrhi::Format::SRGBA8_UNORM;
    desc.debugName = "CaptureTarget";
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.keepInitialState = true;
    nvrhi::TextureHandle target = gpu->createTexture(desc);
    nvrhi::StagingTextureHandle readback = gpu->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    if (!target || !readback) return makeError("could not create a {}x{} capture target", extent.width, extent.height);

    FrameResources& frame = beginFrame();
    ImportedImage output;
    output.texture = target;
    std::vector<nvrhi::CommandListHandle> lists =
        record(frame, scene, camera, settings, output, [&](RenderGraph& graph, RGImage out) {
            if (extraPasses) extraPasses(graph, out);
            graph.addPass(
                "Readback", [&](PassBuilder& b) { b.copyFrom(out); },
                [&readback, out](PassContext& ctx) {  // `out` by value: this runs after the setup returned
                    ctx.cmd->copyTexture(readback, nvrhi::TextureSlice(), ctx.texture(out), nvrhi::TextureSlice());
                });
        });
    submit(frame, lists);
    finish(frame);
    advance();
    ++frameNumber_;

    std::size_t pitch = 0;
    const auto* mapped = static_cast<const std::uint8_t*>(
        gpu->mapStagingTexture(readback, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
    if (mapped == nullptr) return makeError("could not read the capture back");
    CapturedImage image;
    image.width = extent.width;
    image.height = extent.height;
    const std::size_t row = static_cast<std::size_t>(extent.width) * 4;
    image.rgba.resize(row * extent.height);
    for (std::uint32_t y = 0; y < extent.height; ++y) std::memcpy(image.rgba.data() + y * row, mapped + y * pitch, row);
    gpu->unmapStagingTexture(readback);
    return image;
}

} // namespace lumora::render
