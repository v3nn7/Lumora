#include "renderer/vulkan/Instance.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace lumora::rhi {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    const char* message = data != nullptr && data->pMessage != nullptr ? data->pMessage : "(null)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log::error("Vulkan", "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        log::warn("Vulkan", "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        log::debug("Vulkan", "{}", message);
    } else {
        log::trace("Vulkan", "{}", message);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT messengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

bool layerAvailable(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::ranges::any_of(layers, [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

} // namespace

std::uint32_t Instance::queryLoaderVersion() {
    std::uint32_t version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&version);
    return version;
}

std::vector<std::string> Instance::availableExtensions() {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    std::vector<std::string> names;
    names.reserve(count);
    for (const auto& p : props) names.emplace_back(p.extensionName);
    return names;
}

Instance::Instance(const InstanceConfig& config) {
    loaderVersion_ = queryLoaderVersion();
    if (loaderVersion_ < kRequiredApiVersion) {
        throw UnsupportedHardwareError(std::format(
            "Lumora requires a Vulkan 1.4 loader, but the installed loader only supports Vulkan {}. "
            "Update the Vulkan runtime / GPU driver.",
            versionString(loaderVersion_)));
    }

    const auto available = availableExtensions();
    auto isAvailable = [&](const std::string& n) { return std::ranges::find(available, n) != available.end(); };

    for (const auto& ext : config.requiredExtensions) {
        if (!isAvailable(ext)) {
            throw UnsupportedHardwareError(std::format("required Vulkan instance extension {} is not available", ext));
        }
        enabled_.push_back(ext);
    }
    for (const auto& ext : config.optionalExtensions) {
        if (isAvailable(ext)) enabled_.push_back(ext);
    }
    debugUtils_ = isAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils_) enabled_.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    std::ranges::sort(enabled_);
    enabled_.erase(std::ranges::unique(enabled_).begin(), enabled_.end());

    std::vector<const char*> layers;
    if (config.enableValidation) {
        if (layerAvailable(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            validation_ = true;
        } else {
            log::warn("Vulkan", "validation requested but {} is not installed", kValidationLayer);
        }
    }

    std::vector<const char*> extPtrs;
    for (const auto& e : enabled_) extPtrs.push_back(e.c_str());

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = config.applicationName.c_str();
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "Lumora Engine";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = kRequiredApiVersion;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extPtrs.size());
    info.ppEnabledExtensionNames = extPtrs.data();
    VkDebugUtilsMessengerCreateInfoEXT dbg = messengerInfo();
    if (validation_ && debugUtils_) {
        info.pNext = &dbg;  // also captures instance creation/destruction messages
    }
    const VkResult created = vkCreateInstance(&info, nullptr, &instance_);
    if (created == VK_ERROR_INCOMPATIBLE_DRIVER) {
        // The loader found no driver that implements Vulkan 1.4 (or none at all).
        throw UnsupportedHardwareError(std::format("no Vulkan driver supports Vulkan {} (vkCreateInstance: {})",
                                                   versionString(kRequiredApiVersion), toString(created)));
    }
    LUMORA_VK_CHECK(created);

    if (validation_ && debugUtils_) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) {
            LUMORA_VK_CHECK(create(instance_, &dbg, nullptr, &messenger_));
        }
    }
    log::info("Vulkan", "instance created (loader {}, validation {}, {} extensions)", versionString(loaderVersion_),
              validation_ ? "on" : "off", enabled_.size());
}

Instance::~Instance() {
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) destroy(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

bool Instance::isExtensionEnabled(std::string_view name) const {
    return std::ranges::find(enabled_, name) != enabled_.end();
}

} // namespace lumora::rhi
