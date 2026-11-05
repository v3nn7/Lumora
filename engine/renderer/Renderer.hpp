#pragma once

#include "core/Error.hpp"
#include "renderer/FrameResources.hpp"
#include "renderer/GpuProfiler.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/vulkan/Device.hpp"
#include "renderer/vulkan/Swapchain.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <vector>

namespace lumora::render {

class RenderContext;

struct FrameStats {
    std::uint64_t frameNumber = 0;
    double cpuRecordMs = 0.0;
    double gpuFrameMs = 0.0;              // from timestamps, kFramesInFlight frames old
    std::vector<GpuTiming> gpuPasses;
    SceneDrawStats scene;
    rhi::Device::MemoryUsage vram;
    VkExtent2D extent{};
};

struct CapturedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // sRGB-encoded RGBA8, rows top to bottom
};

/// A presentable surface (window) the renderer can draw into.
class PresentTarget {
public:
    PresentTarget(RenderContext& context, VkSurfaceKHR surface, VkExtent2D extent, bool vsync);
    ~PresentTarget();
    PresentTarget(const PresentTarget&) = delete;
    PresentTarget& operator=(const PresentTarget&) = delete;

    void resize(VkExtent2D extent) noexcept;
    void setVsync(bool vsync) noexcept;
    [[nodiscard]] const rhi::Swapchain& swapchain() const noexcept { return *swapchain_; }

private:
    friend class Renderer;
    std::unique_ptr<rhi::Swapchain> swapchain_;
    VkExtent2D desired_{};
    bool dirty_ = false;
};

enum class FrameResult { Rendered, Skipped, SurfaceLost };

/// Frame orchestration: frames in flight, command recording, render graph execution,
/// submission/presentation, GPU timing and offscreen capture.
///
/// The scene's world transforms must be current (Scene::updateWorldTransforms) before rendering.
class Renderer {
public:
    static constexpr std::uint32_t kFramesInFlight = 2;

    explicit Renderer(RenderContext& context);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Renders and presents one frame. Throws rhi::DeviceLostError if the GPU is lost.
    FrameResult render(PresentTarget& target, const Scene& scene, const CameraView& camera,
                       const RenderSettings& settings);
    /// Renders offscreen and reads the tonemapped result back (blocking).
    [[nodiscard]] Result<CapturedImage> capture(const Scene& scene, const CameraView& camera, VkExtent2D extent,
                                                const RenderSettings& settings);

    [[nodiscard]] const FrameStats& stats() const noexcept { return stats_; }
    [[nodiscard]] RenderContext& context() noexcept { return ctx_; }
    void waitIdle();

private:
    FrameResources& beginFrame();
    SceneDrawStats record(FrameResources& frame, const Scene& scene, const CameraView& camera,
                          const RenderSettings& settings, const ImportedImage& output,
                          const std::function<void(RenderGraph&, RGImage)>& extraPasses);
    void submit(FrameResources& frame, VkSemaphore waitAcquired, VkSemaphore signalPresent);
    void advance() noexcept;

    RenderContext& ctx_;
    SceneRenderer sceneRenderer_;
    rhi::UniqueDescriptorPool descriptorPool_;
    std::array<FrameResources, kFramesInFlight> frames_;
    std::uint32_t slot_ = 0;
    std::uint64_t frameNumber_ = 0;
    FrameStats stats_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace lumora::render
