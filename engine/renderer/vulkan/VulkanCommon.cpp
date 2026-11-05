#include "renderer/vulkan/VulkanCommon.hpp"

#include <format>

namespace lumora::rhi {

std::string_view toString(VkResult result) noexcept {
    switch (result) {
#define LUMORA_CASE(x) case x: return #x;
        LUMORA_CASE(VK_SUCCESS)
        LUMORA_CASE(VK_NOT_READY)
        LUMORA_CASE(VK_TIMEOUT)
        LUMORA_CASE(VK_EVENT_SET)
        LUMORA_CASE(VK_EVENT_RESET)
        LUMORA_CASE(VK_INCOMPLETE)
        LUMORA_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        LUMORA_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        LUMORA_CASE(VK_ERROR_INITIALIZATION_FAILED)
        LUMORA_CASE(VK_ERROR_DEVICE_LOST)
        LUMORA_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        LUMORA_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        LUMORA_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        LUMORA_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        LUMORA_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        LUMORA_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        LUMORA_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        LUMORA_CASE(VK_ERROR_FRAGMENTED_POOL)
        LUMORA_CASE(VK_ERROR_UNKNOWN)
        LUMORA_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        LUMORA_CASE(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        LUMORA_CASE(VK_ERROR_FRAGMENTATION)
        LUMORA_CASE(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
        LUMORA_CASE(VK_PIPELINE_COMPILE_REQUIRED)
        LUMORA_CASE(VK_ERROR_SURFACE_LOST_KHR)
        LUMORA_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        LUMORA_CASE(VK_SUBOPTIMAL_KHR)
        LUMORA_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        LUMORA_CASE(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR)
        LUMORA_CASE(VK_ERROR_VALIDATION_FAILED_EXT)
#undef LUMORA_CASE
        default: return "VK_RESULT_UNKNOWN";
    }
}

std::string versionString(std::uint32_t v) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

void throwVulkanError(VkResult result, const char* expression, std::source_location loc) {
    const std::string message =
        std::format("{} failed with {} ({}:{})", expression, toString(result), loc.file_name(), loc.line());
    if (result == VK_ERROR_DEVICE_LOST) {
        throw DeviceLostError(message);
    }
    throw VulkanError(message, result);
}

} // namespace lumora::rhi
