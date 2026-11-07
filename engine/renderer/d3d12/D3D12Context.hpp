#pragma once

#include "renderer/vulkan/Device.hpp"     // rhi::GpuCapabilities, rhi::Device::MemoryUsage
#include "renderer/vulkan/Swapchain.hpp"  // rhi::PresentPolicy

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace lumora::render::d3d12 {

/// Whether this build has the Direct3D 12 back end (Windows, LUMORA_WITH_D3D12).
[[nodiscard]] bool compiledIn() noexcept;

struct Options {
    bool validation = false;                    // the D3D12 debug layer
    std::optional<std::uint32_t> adapterIndex;  // DXGI adapter order; default: the first hardware one
    bool warp = false;                          // the software rasterizer (CI machines without a GPU)
};

/// A Direct3D 12 device chosen and created by Lumora (DXGI adapter, device, graphics queue)
/// with NVRHI on top. Throws rhi::UnsupportedHardwareError when there is no D3D12 adapter.
class Context {
public:
    Context(const Options& options, nvrhi::IMessageCallback* messages, std::uint32_t bindlessCapacity);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    [[nodiscard]] nvrhi::IDevice* device() const noexcept;
    [[nodiscard]] const rhi::GpuCapabilities& caps() const noexcept;
    [[nodiscard]] rhi::Device::MemoryUsage memoryUsage() const;
    /// Native objects for swapchains (IDXGIFactory4*, ID3D12CommandQueue*), as void*.
    [[nodiscard]] void* dxgiFactory() const noexcept;
    [[nodiscard]] void* graphicsQueue() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A DXGI flip-model swapchain over a window (HWND), three RGBA8 buffers.
class Swapchain {
public:
    Swapchain(Context& context, void* window, VkExtent2D extent, rhi::PresentPolicy policy);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// New size (the GPU must be idle and every view of the buffers released). False when the
    /// window has no area (minimized).
    bool resize(VkExtent2D extent);
    void setPolicy(rhi::PresentPolicy policy) noexcept { policy_ = policy; }
    [[nodiscard]] rhi::PresentPolicy policy() const noexcept { return policy_; }
    [[nodiscard]] bool presentBlocks() const noexcept { return policy_ != rhi::PresentPolicy::Uncapped; }
    [[nodiscard]] std::uint32_t imageCount() const noexcept;
    [[nodiscard]] std::uint32_t currentImage() const;
    [[nodiscard]] void* image(std::uint32_t index) const;  // ID3D12Resource*
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] static nvrhi::Format format() noexcept { return nvrhi::Format::RGBA8_UNORM; }
    /// False when the device was removed.
    bool present();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VkExtent2D extent_{};
    rhi::PresentPolicy policy_ = rhi::PresentPolicy::Vsync;
};

} // namespace lumora::render::d3d12
