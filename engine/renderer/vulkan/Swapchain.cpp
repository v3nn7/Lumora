#include "renderer/vulkan/Swapchain.hpp"

#include "core/Log.hpp"
#include "renderer/vulkan/Device.hpp"

#include <algorithm>
#include <format>

namespace lumora::rhi {

namespace {

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    // Prefer sRGB formats so the hardware performs the linear -> sRGB encode on store.
    for (VkFormat preferred : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
        for (const auto& f : formats) {
            if (f.format == preferred && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
        }
    }
    for (VkFormat fallback : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (const auto& f : formats) {
            if (f.format == fallback && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
        }
    }
    return formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes, PresentPolicy policy) {
    auto pick = [&](std::initializer_list<VkPresentModeKHR> order) {
        for (VkPresentModeKHR m : order) {
            if (std::ranges::find(modes, m) != modes.end()) return m;
        }
        return VK_PRESENT_MODE_FIFO_KHR;  // the only mode every implementation must support
    };
    switch (policy) {
        case PresentPolicy::Vsync: return pick({VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR});
        case PresentPolicy::Uncapped:
            return pick({VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR});
        case PresentPolicy::StrictFifo: return VK_PRESENT_MODE_FIFO_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char* presentModeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "other";
    }
}

} // namespace

Swapchain::Swapchain(Device& device, VkSurfaceKHR surface, VkExtent2D desiredExtent, PresentPolicy policy)
    : device_(device), surface_(surface), policy_(policy) {
    if (!device_.caps().swapchain) {
        throw UnsupportedHardwareError("device does not support VK_KHR_swapchain");
    }
    if (!device_.supportsPresent(surface_)) {
        throw UnsupportedHardwareError(std::format("{}: graphics queue family cannot present to this surface",
                                                   device_.caps().name));
    }
    recreate(desiredExtent);
}

Swapchain::~Swapchain() {
    if (swapchain_ != VK_NULL_HANDLE) {
        // Present operations may still reference the swapchain or its semaphores.
        vkDeviceWaitIdle(device_.handle());
        destroyViews();
        vkDestroySwapchainKHR(device_.handle(), swapchain_, nullptr);
    }
}

void Swapchain::destroyViews() noexcept {
    for (VkImageView v : views_) vkDestroyImageView(device_.handle(), v, nullptr);
    views_.clear();
    images_.clear();
    presentSemaphores_.clear();
}

bool Swapchain::recreate(VkExtent2D desired) {
    VkPhysicalDevice pd = device_.physical();
    VkSurfaceCapabilitiesKHR caps{};
    const VkResult capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface_, &caps);
    if (capsResult == VK_ERROR_SURFACE_LOST_KHR) return false;
    LUMORA_VK_CHECK(capsResult);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {  // surface size is determined by the swapchain (e.g. Wayland)
        extent.width = std::clamp(desired.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(desired.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &count, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &count, modes.data());
    if (formats.empty()) {
        throw VulkanError("surface reports no formats", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }

    const VkSurfaceFormatKHR surfaceFormat = chooseFormat(formats);
    presentMode_ = choosePresentMode(modes, policy_);

    std::uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (auto a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                       VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & a) {
                alpha = a;
                break;
            }
        }
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                            : caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = presentMode_;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;

    // In-flight frames may still use the old images; the caller waits for frame fences, and we
    // additionally idle here because recreation is rare and simplicity beats a deferred scheme.
    vkDeviceWaitIdle(device_.handle());

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateSwapchainKHR(device_.handle(), &info, nullptr, &newSwapchain));
    destroyViews();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_.handle(), swapchain_, nullptr);
    }
    swapchain_ = newSwapchain;
    format_ = surfaceFormat.format;
    colorSpace_ = surfaceFormat.colorSpace;
    extent_ = extent;

    vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &count, images_.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        LUMORA_VK_CHECK(vkCreateImageView(device_.handle(), &viewInfo, nullptr, &view));
        views_.push_back(view);
        presentSemaphores_.push_back(createBinarySemaphore(device_, "SwapchainPresent"));
    }
    std::string available;
    for (VkPresentModeKHR m : modes) available += std::string(available.empty() ? "" : "/") + presentModeName(m);
    log::info("Vulkan", "swapchain {}x{}, {} images, format {}, {} (available: {})", extent_.width, extent_.height,
              count, static_cast<int>(format_), presentModeName(presentMode_), available);
    return true;
}

SwapchainStatus Swapchain::acquire(VkSemaphore signal, std::uint32_t& imageIndex, std::uint64_t timeoutNs) {
    const VkResult r = vkAcquireNextImageKHR(device_.handle(), swapchain_, timeoutNs, signal, VK_NULL_HANDLE, &imageIndex);
    switch (r) {
        case VK_SUCCESS: return SwapchainStatus::Ok;
        case VK_SUBOPTIMAL_KHR: return SwapchainStatus::Suboptimal;
        case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainStatus::OutOfDate;
        case VK_ERROR_SURFACE_LOST_KHR: return SwapchainStatus::SurfaceLost;
        case VK_TIMEOUT:
        case VK_NOT_READY: return SwapchainStatus::Timeout;
        default: LUMORA_VK_CHECK(r); return SwapchainStatus::OutOfDate;
    }
}

SwapchainStatus Swapchain::present(std::uint32_t imageIndex) {
    VkSemaphore wait = presentSemaphores_[imageIndex].get();
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &imageIndex;
    const VkResult r = device_.present(info);
    switch (r) {
        case VK_SUCCESS: return SwapchainStatus::Ok;
        case VK_SUBOPTIMAL_KHR: return SwapchainStatus::Suboptimal;
        case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainStatus::OutOfDate;
        case VK_ERROR_SURFACE_LOST_KHR: return SwapchainStatus::SurfaceLost;
        default: LUMORA_VK_CHECK(r); return SwapchainStatus::OutOfDate;
    }
}

} // namespace lumora::rhi
