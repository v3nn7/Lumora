#include "renderer/GpuProfiler.hpp"

namespace lumora::render {

GpuProfiler::GpuProfiler(nvrhi::IDevice* device, bool enabled) : device_(device), enabled_(enabled) {}

nvrhi::ITimerQuery* GpuProfiler::query(std::uint32_t index) {
    while (queries_.size() <= index) queries_.push_back(device_->createTimerQuery());
    return queries_[index];
}

void GpuProfiler::beginFrame(nvrhi::ICommandList* cmd) {
    used_ = 1;
    names_.assign(1, "Frame");
    recorded_ = enabled_;
    if (enabled_) cmd->beginTimerQuery(query(0));
}

std::uint32_t GpuProfiler::beginScope(nvrhi::ICommandList* cmd, const std::string& name) {
    if (!recorded_) return UINT32_MAX;
    const std::uint32_t scope = used_++;
    names_.push_back(name);
    cmd->beginTimerQuery(query(scope));
    return scope;
}

void GpuProfiler::endScope(nvrhi::ICommandList* cmd, std::uint32_t scope) {
    if (!recorded_ || scope == UINT32_MAX) return;
    cmd->endTimerQuery(query(scope));
}

void GpuProfiler::endFrame(nvrhi::ICommandList* cmd) {
    if (recorded_) cmd->endTimerQuery(query(0));
}

void GpuProfiler::resolve() {
    results_.clear();
    if (!recorded_) return;
    recorded_ = false;
    frameMs_ = static_cast<double>(device_->getTimerQueryTime(queries_[0])) * 1000.0;
    for (std::uint32_t i = 1; i < used_; ++i) {
        results_.push_back({names_[i], static_cast<double>(device_->getTimerQueryTime(queries_[i])) * 1000.0});
    }
    for (std::uint32_t i = 0; i < used_; ++i) device_->resetTimerQuery(queries_[i]);
}

} // namespace lumora::render
