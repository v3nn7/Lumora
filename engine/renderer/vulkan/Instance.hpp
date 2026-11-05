#pragma once

#include "renderer/vulkan/VulkanCommon.hpp"

#include <string>
#include <vector>

namespace lumora::rhi {

struct InstanceConfig {
    std::string applicationName = "Lumora";
    bool enableValidation = false;
    /// Must be present (e.g. platform surface extensions). Missing ones fail creation.
    std::vector<std::string> requiredExtensions;
    /// Enabled when available.
    std::vector<std::string> optionalExtensions;
};

/// VkInstance with Vulkan 1.4 API, optional Khronos validation and debug-utils messenger.
class Instance {
public:
    static constexpr std::uint32_t kRequiredApiVersion = VK_API_VERSION_1_4;

    explicit Instance(const InstanceConfig& config);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    [[nodiscard]] VkInstance handle() const noexcept { return instance_; }
    [[nodiscard]] bool validationEnabled() const noexcept { return validation_; }
    [[nodiscard]] bool debugUtilsEnabled() const noexcept { return debugUtils_; }
    [[nodiscard]] std::uint32_t loaderVersion() const noexcept { return loaderVersion_; }
    [[nodiscard]] bool isExtensionEnabled(std::string_view name) const;
    [[nodiscard]] const std::vector<std::string>& enabledExtensions() const noexcept { return enabled_; }

    /// Instance extensions the loader advertises.
    [[nodiscard]] static std::vector<std::string> availableExtensions();
    [[nodiscard]] static std::uint32_t queryLoaderVersion();

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    std::vector<std::string> enabled_;
    std::uint32_t loaderVersion_ = 0;
    bool validation_ = false;
    bool debugUtils_ = false;
};

} // namespace lumora::rhi
