#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lumora::rhi {

/// Thrown when Vulkan initialization or a non-recoverable call fails.
class VulkanError : public std::runtime_error {
public:
    VulkanError(const std::string& message, VkResult result) : std::runtime_error(message), result_(result) {}
    [[nodiscard]] VkResult result() const noexcept { return result_; }

private:
    VkResult result_;
};

/// The GPU stopped responding (driver reset, hang, removal). All Vulkan objects are unusable.
class DeviceLostError : public VulkanError {
public:
    explicit DeviceLostError(const std::string& message) : VulkanError(message, VK_ERROR_DEVICE_LOST) {}
};

/// Hardware/driver cannot satisfy Lumora's minimum requirements (e.g. no Vulkan 1.4).
class UnsupportedHardwareError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string_view toString(VkResult result) noexcept;
[[nodiscard]] std::string versionString(std::uint32_t apiVersion);

[[noreturn]] void throwVulkanError(VkResult result, const char* expression, std::source_location loc);

inline void check(VkResult result, const char* expression,
                  std::source_location loc = std::source_location::current()) {
    if (result != VK_SUCCESS) [[unlikely]] {
        throwVulkanError(result, expression, loc);
    }
}

} // namespace lumora::rhi

#define LUMORA_VK_CHECK(expr) ::lumora::rhi::check((expr), #expr)
