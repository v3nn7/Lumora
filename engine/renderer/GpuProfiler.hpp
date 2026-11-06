#pragma once

#include "renderer/Gpu.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lumora::render {

struct GpuTiming {
    std::string name;
    double milliseconds = 0.0;
};

/// Per-frame-slot GPU timer queries, one per render-graph pass plus the whole frame. Results
/// are read after the slot's frame completed, i.e. they describe the frame rendered
/// `framesInFlight` frames ago.
class GpuProfiler {
public:
    GpuProfiler(nvrhi::IDevice* device, bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void beginFrame(nvrhi::ICommandList* cmd);
    [[nodiscard]] std::uint32_t beginScope(nvrhi::ICommandList* cmd, const std::string& name);
    void endScope(nvrhi::ICommandList* cmd, std::uint32_t scope);
    void endFrame(nvrhi::ICommandList* cmd);
    /// Call after the frame completed on the GPU.
    void resolve();
    [[nodiscard]] const std::vector<GpuTiming>& results() const noexcept { return results_; }
    [[nodiscard]] double frameMilliseconds() const noexcept { return frameMs_; }

private:
    nvrhi::ITimerQuery* query(std::uint32_t index);

    nvrhi::IDevice* device_;
    bool enabled_ = false;
    std::vector<nvrhi::TimerQueryHandle> queries_;  // 0 = whole frame, then the scopes
    std::vector<std::string> names_;
    std::uint32_t used_ = 0;
    bool recorded_ = false;
    std::vector<GpuTiming> results_;
    double frameMs_ = 0.0;
};

} // namespace lumora::render
