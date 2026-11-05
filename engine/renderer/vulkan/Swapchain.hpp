#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <vector>

namespace lumora::rhi {

class Device;

enum class SwapchainStatus { Ok, Suboptimal, OutOfDate, SurfaceLost, Timeout };

/// Presentation policy. Vsync prefers MAILBOX (tear-free and never blocks the calling thread)
/// and falls back to FIFO; Uncapped prefers IMMEDIATE. Embedding hosts that render on the UI
/// thread must avoid blocking FIFO presents: on Wayland a FIFO present waits for compositor
/// frame callbacks, which never arrive while the window is hidden.
enum class PresentPolicy { Vsync, Uncapped, StrictFifo };

/// VkSwapchainKHR over an externally owned surface (the windowing layer owns the VkSurfaceKHR).
/// Present semaphores are per swapchain image, which is the only reuse-safe scheme for present.
class Swapchain {
public:
    Swapchain(Device& device, VkSurfaceKHR surface, VkExtent2D desiredExtent, PresentPolicy policy);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// Rebuilds for a new size / mode. Returns false when the surface has zero area (minimized).
    bool recreate(VkExtent2D desiredExtent);
    void setPolicy(PresentPolicy policy) noexcept { policy_ = policy; }
    [[nodiscard]] PresentPolicy policy() const noexcept { return policy_; }
    /// True when vkQueuePresentKHR itself throttles to the display (FIFO).
    [[nodiscard]] bool presentBlocks() const noexcept { return presentMode_ == VK_PRESENT_MODE_FIFO_KHR || presentMode_ == VK_PRESENT_MODE_FIFO_RELAXED_KHR; }

    /// A bounded timeout keeps the UI thread responsive when a compositor withholds images
    /// (e.g. a hidden Wayland window under FIFO).
    [[nodiscard]] SwapchainStatus acquire(VkSemaphore signal, std::uint32_t& imageIndex,
                                          std::uint64_t timeoutNs = 20'000'000);
    [[nodiscard]] SwapchainStatus present(std::uint32_t imageIndex);

    [[nodiscard]] bool valid() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] std::uint32_t imageCount() const noexcept { return static_cast<std::uint32_t>(images_.size()); }
    [[nodiscard]] VkImage image(std::uint32_t i) const { return images_[i]; }
    [[nodiscard]] VkImageView view(std::uint32_t i) const { return views_[i]; }
    [[nodiscard]] VkSemaphore presentSemaphore(std::uint32_t i) const { return presentSemaphores_[i].get(); }
    [[nodiscard]] VkPresentModeKHR presentMode() const noexcept { return presentMode_; }

private:
    void destroyViews() noexcept;

    Device& device_;
    VkSurfaceKHR surface_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    PresentPolicy policy_ = PresentPolicy::Vsync;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<UniqueSemaphore> presentSemaphores_;
};

} // namespace lumora::rhi
