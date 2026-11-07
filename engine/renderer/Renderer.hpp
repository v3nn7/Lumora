#pragma once

#include "assets/Image.hpp"
#include "core/Error.hpp"
#include "renderer/FrameResources.hpp"
#include "renderer/GpuProfiler.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/d3d12/D3D12Context.hpp"
#include "renderer/vulkan/Device.hpp"
#include "renderer/vulkan/Swapchain.hpp"

#include <array>
#include <filesystem>
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

/// A presentable surface (window) the renderer can draw into: a Vulkan swapchain over a
/// VkSurfaceKHR, or a DXGI swapchain over a window handle (Direct3D 12).
class PresentTarget {
public:
    PresentTarget(RenderContext& context, VkSurfaceKHR surface, VkExtent2D extent, rhi::PresentPolicy policy);
    /// Direct3D 12: `window` is the HWND.
    PresentTarget(RenderContext& context, void* window, VkExtent2D extent, rhi::PresentPolicy policy);
    ~PresentTarget();
    PresentTarget(const PresentTarget&) = delete;
    PresentTarget& operator=(const PresentTarget&) = delete;

    void resize(VkExtent2D extent) noexcept;
    void setPolicy(rhi::PresentPolicy policy) noexcept;
    /// Whether presenting waits for the display (FIFO / vsync): then the caller need not pace.
    [[nodiscard]] bool presentBlocks() const noexcept;

private:
    friend class Renderer;
    /// NVRHI textures over the swapchain's images (made on first use after each recreate).
    nvrhi::ITexture* image(std::uint32_t index);
    void releaseImages();

    RenderContext& ctx_;
    std::unique_ptr<rhi::Swapchain> swapchain_;          // Vulkan
    std::unique_ptr<d3d12::Swapchain> d3d12Swapchain_;   // Direct3D 12
    std::vector<nvrhi::TextureHandle> images_;
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
    /// Renders offscreen and reads the tonemapped result back (blocking). `extraPasses` adds
    /// passes after the scene's (they may write the output image).
    [[nodiscard]] Result<CapturedImage> capture(const Scene& scene, const CameraView& camera, VkExtent2D extent,
                                                const RenderSettings& settings,
                                                const std::function<void(RenderGraph&, RGImage)>& extraPasses = {});

    /// Renders offscreen and reads the scene's linear HDR radiance back (blocking); for baking
    /// reflection probes. `settings.linearOutput` is implied.
    [[nodiscard]] Result<HdrImage> captureHdr(const Scene& scene, const CameraView& camera, VkExtent2D extent,
                                              RenderSettings settings);

    [[nodiscard]] const FrameStats& stats() const noexcept { return stats_; }
    [[nodiscard]] RenderContext& context() noexcept { return ctx_; }
    void waitIdle();
    /// A file changed on disk: drops the GPU meshes and textures made from it (they upload
    /// again on next use). Waits for the GPU first.
    void invalidateAssetFile(const std::filesystem::path& absoluteFile);

private:
    FrameResources& beginFrame();
    /// Records the frame; the returned command lists are closed and ready to execute.
    std::vector<nvrhi::CommandListHandle> record(FrameResources& frame, const Scene& scene, const CameraView& camera,
                                                 const RenderSettings& settings, const ImportedImage& output,
                                                 const std::function<void(RenderGraph&, RGImage)>& extraPasses);
    void submit(FrameResources& frame, std::vector<nvrhi::CommandListHandle>& lists);
    FrameResult renderD3D12(PresentTarget& target, const Scene& scene, const CameraView& camera, const RenderSettings& settings);
    void finish(FrameResources& frame);
    void advance() noexcept;

    RenderContext& ctx_;
    SceneRenderer sceneRenderer_;
    FramebufferCache framebuffers_;
    std::array<FrameResources, kFramesInFlight> frames_;
    std::uint32_t slot_ = 0;
    std::uint64_t frameNumber_ = 0;
    FrameStats stats_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace lumora::render
