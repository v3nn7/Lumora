#include "renderer/vulkan/Device.hpp"

#include "core/Log.hpp"
#include "renderer/vulkan/Instance.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace lumora::rhi {

namespace {

std::vector<std::string> deviceExtensions(VkPhysicalDevice pd) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, props.data());
    std::vector<std::string> names;
    for (const auto& p : props) names.emplace_back(p.extensionName);
    return names;
}

std::optional<std::uint32_t> findGraphicsFamily(VkPhysicalDevice pd, bool& timestamps) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            timestamps = families[i].timestampValidBits > 0;
            return i;
        }
    }
    return std::nullopt;
}

struct FeatureChain {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};

    FeatureChain() { link(); }
    FeatureChain(const FeatureChain&) = delete;
    void link() {
        core.pNext = &v11;
        v11.pNext = &v12;
        v12.pNext = &v13;
        v13.pNext = &v14;
        v14.pNext = nullptr;
    }
};

/// Returns an empty string if all required features are supported, otherwise the first missing one.
std::string missingRequiredFeature(const FeatureChain& f) {
    if (!f.v13.dynamicRendering) return "dynamicRendering";
    if (!f.v13.synchronization2) return "synchronization2";
    if (!f.v12.timelineSemaphore) return "timelineSemaphore";
    if (!f.v12.descriptorIndexing) return "descriptorIndexing";
    if (!f.v12.runtimeDescriptorArray) return "runtimeDescriptorArray";
    if (!f.v12.descriptorBindingPartiallyBound) return "descriptorBindingPartiallyBound";
    if (!f.v12.descriptorBindingSampledImageUpdateAfterBind) return "descriptorBindingSampledImageUpdateAfterBind";
    if (!f.v12.descriptorBindingUpdateUnusedWhilePending) return "descriptorBindingUpdateUnusedWhilePending";
    if (!f.v13.shaderDemoteToHelperInvocation) return "shaderDemoteToHelperInvocation";
    return {};
}

} // namespace

std::string GpuCapabilities::typeName() const {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU (software)";
        default: return "other";
    }
}

std::vector<DeviceCandidate> enumerateDevices(VkInstance instance, bool requireSwapchain) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    std::vector<DeviceCandidate> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        DeviceCandidate c;
        c.physical = devices[i];
        c.index = i;

        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceVulkan12Properties props12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        driver.pNext = &props12;
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &driver;
        vkGetPhysicalDeviceProperties2(devices[i], &props);

        GpuCapabilities& caps = c.caps;
        caps.name = props.properties.deviceName;
        caps.driverName = driver.driverName;
        caps.driverInfo = driver.driverInfo;
        caps.apiVersion = props.properties.apiVersion;
        caps.vendorId = props.properties.vendorID;
        caps.deviceId = props.properties.deviceID;
        caps.type = props.properties.deviceType;
        caps.timestampPeriodNs = props.properties.limits.timestampPeriod;
        caps.maxPushConstantsSize = props.properties.limits.maxPushConstantsSize;
        caps.maxAnisotropy = props.properties.limits.maxSamplerAnisotropy;
        caps.maxImageDimension2D = props.properties.limits.maxImageDimension2D;
        caps.maxBindlessSampledImages = std::min(props12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                                 props12.maxDescriptorSetUpdateAfterBindSampledImages);
        caps.maxBindlessSamplers = std::min(props12.maxPerStageDescriptorUpdateAfterBindSamplers,
                                            props12.maxDescriptorSetUpdateAfterBindSamplers);

        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(devices[i], &mem);
        for (std::uint32_t h = 0; h < mem.memoryHeapCount; ++h) {
            if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                caps.deviceLocalBytes += mem.memoryHeaps[h].size;
            }
        }

        const auto exts = deviceExtensions(devices[i]);
        auto has = [&](const char* n) { return std::ranges::find(exts, n) != exts.end(); };
        caps.swapchain = has(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        caps.memoryBudget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        caps.rayTracingPipeline = has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        caps.accelerationStructure = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        caps.rayQuery = has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        caps.meshShader = has(VK_EXT_MESH_SHADER_EXTENSION_NAME);

        FeatureChain features;
        vkGetPhysicalDeviceFeatures2(devices[i], &features.core);
        caps.samplerAnisotropy = features.core.features.samplerAnisotropy == VK_TRUE;
        caps.textureCompressionBC = features.core.features.textureCompressionBC == VK_TRUE;

        bool queueTimestamps = false;
        const auto family = findGraphicsFamily(devices[i], queueTimestamps);
        caps.timestamps = queueTimestamps && caps.timestampPeriodNs > 0.0f;

        if (caps.apiVersion < Instance::kRequiredApiVersion) {
            c.rejectReason = std::format("supports Vulkan {} (1.4 required)", versionString(caps.apiVersion));
        } else if (!family) {
            c.rejectReason = "no graphics+compute queue family";
        } else if (requireSwapchain && !caps.swapchain) {
            c.rejectReason = "VK_KHR_swapchain not supported";
        } else if (auto missing = missingRequiredFeature(features); !missing.empty()) {
            c.rejectReason = "missing required feature " + missing;
        } else {
            c.suitable = true;
        }

        switch (caps.type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: c.score = 1'000'000; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: c.score = 500'000; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: c.score = 100'000; break;
            default: c.score = 1'000; break;
        }
        c.score += static_cast<long long>(caps.deviceLocalBytes >> 20);  // MiB of VRAM as tie-breaker
        result.push_back(std::move(c));
    }
    return result;
}

Device::Device(const Instance& instance, const DeviceSelection& selection) : instance_(instance.handle()) {
    const auto candidates = enumerateDevices(instance_, selection.requireSwapchain);
    if (candidates.empty()) {
        throw UnsupportedHardwareError("no Vulkan physical devices found");
    }
    const DeviceCandidate* chosen = nullptr;
    if (selection.preferredIndex) {
        const auto idx = *selection.preferredIndex;
        if (idx >= candidates.size()) {
            throw UnsupportedHardwareError(std::format("GPU index {} requested but only {} device(s) exist", idx,
                                                       candidates.size()));
        }
        if (!candidates[idx].suitable) {
            throw UnsupportedHardwareError(std::format("requested GPU {} ({}) is unsuitable: {}", idx,
                                                       candidates[idx].caps.name, candidates[idx].rejectReason));
        }
        chosen = &candidates[idx];
    } else {
        for (const auto& c : candidates) {
            if (c.suitable && (chosen == nullptr || c.score > chosen->score)) chosen = &c;
        }
    }
    if (chosen == nullptr) {
        std::string details;
        for (const auto& c : candidates) {
            details += std::format("\n  [{}] {} ({}): {}", c.index, c.caps.name, c.caps.driverName, c.rejectReason);
        }
        throw UnsupportedHardwareError("no GPU satisfies Lumora's requirements (Vulkan 1.4 + dynamic rendering, "
                                       "synchronization2, descriptor indexing):" + details);
    }
    for (const auto& c : candidates) {
        log::info("Vulkan", "GPU [{}] {} - {} - Vulkan {} - {}", c.index, c.caps.name, c.caps.typeName(),
                  versionString(c.caps.apiVersion), c.suitable ? "suitable" : c.rejectReason);
    }

    physical_ = chosen->physical;
    caps_ = chosen->caps;
    bool queueTimestamps = false;
    graphicsFamily_ = *findGraphicsFamily(physical_, queueTimestamps);

    // Enable required features plus optional ones that are supported.
    FeatureChain supported;
    vkGetPhysicalDeviceFeatures2(physical_, &supported.core);
    FeatureChain enable;
    enable.core.features.samplerAnisotropy = supported.core.features.samplerAnisotropy;
    enable.core.features.textureCompressionBC = supported.core.features.textureCompressionBC;
    enable.core.features.fillModeNonSolid = supported.core.features.fillModeNonSolid;
    enable.core.features.shaderInt64 = supported.core.features.shaderInt64;
    // Compute passes write storage images declared without a format (glslc's HLSL has no
    // format qualifier); supported by every current desktop driver.
    enable.core.features.shaderStorageImageWriteWithoutFormat = supported.core.features.shaderStorageImageWriteWithoutFormat;
    enable.core.features.shaderStorageImageReadWithoutFormat = supported.core.features.shaderStorageImageReadWithoutFormat;
    enable.v11.shaderDrawParameters = supported.v11.shaderDrawParameters;
    enable.v12.timelineSemaphore = VK_TRUE;
    enable.v12.descriptorIndexing = VK_TRUE;
    enable.v12.runtimeDescriptorArray = VK_TRUE;
    enable.v12.descriptorBindingPartiallyBound = VK_TRUE;
    enable.v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    enable.v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    enable.v12.shaderSampledImageArrayNonUniformIndexing = supported.v12.shaderSampledImageArrayNonUniformIndexing;
    enable.v12.scalarBlockLayout = supported.v12.scalarBlockLayout;
    enable.v12.bufferDeviceAddress = supported.v12.bufferDeviceAddress;
    enable.v12.hostQueryReset = supported.v12.hostQueryReset;
    enable.v13.dynamicRendering = VK_TRUE;
    enable.v13.synchronization2 = VK_TRUE;
    enable.v13.maintenance4 = supported.v13.maintenance4;
    enable.v13.shaderDemoteToHelperInvocation = supported.v13.shaderDemoteToHelperInvocation;
    enable.v14.maintenance5 = supported.v14.maintenance5;
    enable.v14.maintenance6 = supported.v14.maintenance6;

    std::vector<const char*> extensions;
    // Headless contexts have no surface extensions, so VK_KHR_swapchain must not be enabled there.
    if (selection.requireSwapchain) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    caps_.swapchain = selection.requireSwapchain;
    if (caps_.memoryBudget) extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = graphicsFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &enable.core;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    LUMORA_VK_CHECK(vkCreateDevice(physical_, &info, nullptr, &device_));
    for (const char* e : extensions) enabledExtensions_.emplace_back(e);
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);

    if (instance.debugUtilsEnabled()) {
        setObjectName_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
        cmdBeginLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
        cmdEndLabel_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
    }

    log::info("Vulkan", "using {} ({}, driver {} {}), Vulkan {}, {} MiB VRAM", caps_.name, caps_.typeName(),
              caps_.driverName, caps_.driverInfo, versionString(caps_.apiVersion), caps_.deviceLocalBytes >> 20);
    log::info("Vulkan", "features: timestamps={} anisotropy={} memoryBudget={} rayTracing={} rayQuery={} meshShader={}",
              caps_.timestamps, caps_.samplerAnisotropy, caps_.memoryBudget,
              caps_.rayTracingPipeline && caps_.accelerationStructure, caps_.rayQuery, caps_.meshShader);
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
}

void Device::submit(const VkSubmitInfo2& info, VkFence fence) {
    std::scoped_lock lock(queueMutex_);
    LUMORA_VK_CHECK(vkQueueSubmit2(graphicsQueue_, 1, &info, fence));
}

VkResult Device::present(const VkPresentInfoKHR& info) {
    std::scoped_lock lock(queueMutex_);
    return vkQueuePresentKHR(graphicsQueue_, &info);
}

void Device::waitIdle() {
    std::scoped_lock lock(queueMutex_);
    LUMORA_VK_CHECK(vkDeviceWaitIdle(device_));
}

bool Device::supportsPresent(VkSurfaceKHR surface) const {
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, graphicsFamily_, surface, &supported);
    return supported == VK_TRUE;
}

bool Device::supportsFormat(VkFormat format, VkFormatFeatureFlags2 features, bool optimalTiling) const {
    VkFormatProperties3 props3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    props.pNext = &props3;
    vkGetPhysicalDeviceFormatProperties2(physical_, format, &props);
    const VkFormatFeatureFlags2 have = optimalTiling ? props3.optimalTilingFeatures : props3.linearTilingFeatures;
    return (have & features) == features;
}

void Device::setName(VkObjectType type, std::uint64_t handle, const char* name) const {
    if (setObjectName_ == nullptr || handle == 0) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    setObjectName_(device_, &info);
}

void Device::beginLabel(VkCommandBuffer cmd, const char* name, const float color[4]) const {
    if (cmdBeginLabel_ == nullptr) return;
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    std::memcpy(label.color, color, sizeof(float) * 4);
    cmdBeginLabel_(cmd, &label);
}

void Device::endLabel(VkCommandBuffer cmd) const {
    if (cmdEndLabel_ != nullptr) cmdEndLabel_(cmd);
}

Device::MemoryUsage Device::deviceLocalMemoryUsage() const {
    // VK_EXT_memory_budget: what this process uses and may use per heap (the driver's numbers).
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    if (caps_.memoryBudget) props.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(physical_, &props);
    const VkPhysicalDeviceMemoryProperties& mem = props.memoryProperties;
    MemoryUsage total;
    for (std::uint32_t h = 0; h < mem.memoryHeapCount; ++h) {
        if ((mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        total.usage += caps_.memoryBudget ? budget.heapUsage[h] : 0;
        total.budget += caps_.memoryBudget ? budget.heapBudget[h] : mem.memoryHeaps[h].size;
    }
    return total;
}

} // namespace lumora::rhi
