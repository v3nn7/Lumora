#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lumora::rhi {
class Device;
}

namespace lumora::render {

struct GpuTiming {
    std::string name;
    double milliseconds = 0.0;
};

/// Per-frame-slot timestamp queries. Results are read back after the slot's fence signals,
/// i.e. they describe the frame rendered `framesInFlight` frames ago.
class GpuProfiler {
public:
    GpuProfiler(const rhi::Device& device, std::uint32_t maxScopes = 64);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void beginFrame(VkCommandBuffer cmd);
    [[nodiscard]] std::uint32_t beginScope(VkCommandBuffer cmd, const std::string& name);
    void endScope(VkCommandBuffer cmd, std::uint32_t scope);
    void endFrame(VkCommandBuffer cmd);
    /// Call after the frame's fence has been waited on.
    void resolve();

    [[nodiscard]] const std::vector<GpuTiming>& results() const noexcept { return results_; }
    [[nodiscard]] double frameMilliseconds() const noexcept { return frameMs_; }

private:
    const rhi::Device& device_;
    bool enabled_ = false;
    std::uint32_t maxQueries_ = 0;
    std::uint32_t used_ = 0;
    bool recorded_ = false;
    double periodNs_ = 1.0;
    rhi::UniqueQueryPool pool_;
    std::vector<std::string> names_;  // scope i uses queries 2+2i, 3+2i; 0/1 = whole frame
    std::vector<GpuTiming> results_;
    double frameMs_ = 0.0;
};

} // namespace lumora::render
