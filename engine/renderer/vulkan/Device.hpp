#pragma once

#include "renderer/vulkan/VulkanCommon.hpp"


#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lumora::rhi {

class Instance;

/// What the physical device can do. Advanced features (ray tracing, mesh shaders) are
/// detected here so higher layers can pick fallback paths; they are not yet used.
/// (Not "DeviceCapabilities": <windows.h> defines that name as a macro.)
struct GpuCapabilities {
    std::string name;
    std::string driverName;
    std::string driverInfo;
    std::uint32_t apiVersion = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    VkDeviceSize deviceLocalBytes = 0;

    bool timestamps = false;
    float timestampPeriodNs = 0.0f;
    bool samplerAnisotropy = false;
    float maxAnisotropy = 1.0f;
    std::uint32_t maxImageDimension2D = 4096;  // guaranteed minimum
    bool memoryBudget = false;
    bool swapchain = false;
    bool rayTracingPipeline = false;
    bool accelerationStructure = false;
    bool rayQuery = false;
    bool meshShader = false;
    std::uint32_t maxPushConstantsSize = 0;
    std::uint32_t maxBindlessSampledImages = 0;
    std::uint32_t maxBindlessSamplers = 0;

    [[nodiscard]] std::string typeName() const;
};

struct DeviceCandidate {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t index = 0;
    GpuCapabilities caps;
    bool suitable = false;
    std::string rejectReason;
    long long score = 0;
};

struct DeviceSelection {
    std::optional<std::uint32_t> preferredIndex;  // --gpu N / LUMORA_GPU
    bool requireSwapchain = true;
};

/// Evaluates every physical device against Lumora's requirements (Vulkan 1.4, dynamic
/// rendering, synchronization2, timeline semaphores, update-after-bind descriptor indexing).
[[nodiscard]] std::vector<DeviceCandidate> enumerateDevices(VkInstance instance, bool requireSwapchain);

/// Logical device, graphics queue and VMA allocator.
class Device {
public:
    Device(const Instance& instance, const DeviceSelection& selection);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkDevice handle() const noexcept { return device_; }
    [[nodiscard]] VkPhysicalDevice physical() const noexcept { return physical_; }
    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return graphicsQueue_; }
    /// Device extensions enabled at creation (NVRHI needs to know them).
    [[nodiscard]] const std::vector<std::string>& enabledExtensions() const noexcept { return enabledExtensions_; }
    [[nodiscard]] std::uint32_t graphicsFamily() const noexcept { return graphicsFamily_; }
    [[nodiscard]] const GpuCapabilities& caps() const noexcept { return caps_; }

    /// Queue access is externally synchronized; all submissions go through these.
    void submit(const VkSubmitInfo2& info, VkFence fence);
    [[nodiscard]] VkResult present(const VkPresentInfoKHR& info);
    void waitIdle();

    [[nodiscard]] bool supportsPresent(VkSurfaceKHR surface) const;
    [[nodiscard]] bool supportsFormat(VkFormat format, VkFormatFeatureFlags2 features, bool optimalTiling = true) const;

    void setName(VkObjectType type, std::uint64_t handle, const char* name) const;
    template <class T>
    void setName(VkObjectType type, T handle, const char* name) const {
        setName(type, reinterpret_cast<std::uint64_t>(handle), name);
    }
    void beginLabel(VkCommandBuffer cmd, const char* name, const float color[4]) const;
    void endLabel(VkCommandBuffer cmd) const;

    /// Bytes used / budget across device-local heaps (requires VK_EXT_memory_budget for accuracy).
    struct MemoryUsage {
        VkDeviceSize usage = 0;
        VkDeviceSize budget = 0;
    };
    [[nodiscard]] MemoryUsage deviceLocalMemoryUsage() const;

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    std::vector<std::string> enabledExtensions_;
    std::uint32_t graphicsFamily_ = 0;
    GpuCapabilities caps_;
    std::mutex queueMutex_;

    PFN_vkSetDebugUtilsObjectNameEXT setObjectName_ = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginLabel_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT cmdEndLabel_ = nullptr;
};

} // namespace lumora::rhi
