#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <vector>

namespace lumora::rhi {

class Device;

enum class SwapchainStatus { Ok, Suboptimal, OutOfDate, SurfaceLost };

/// VkSwapchainKHR over an externally owned surface (the windowing layer owns the VkSurfaceKHR).
/// Present semaphores are per swapchain image, which is the only reuse-safe scheme for present.
class Swapchain {
public:
    Swapchain(Device& device, VkSurfaceKHR surface, VkExtent2D desiredExtent, bool vsync);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// Rebuilds for a new size / mode. Returns false when the surface has zero area (minimized).
    bool recreate(VkExtent2D desiredExtent);
    void setVsync(bool vsync) noexcept { vsync_ = vsync; }
    [[nodiscard]] bool vsync() const noexcept { return vsync_; }

    [[nodiscard]] SwapchainStatus acquire(VkSemaphore signal, std::uint32_t& imageIndex);
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
    bool vsync_ = true;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<UniqueSemaphore> presentSemaphores_;
};

} // namespace lumora::rhi
