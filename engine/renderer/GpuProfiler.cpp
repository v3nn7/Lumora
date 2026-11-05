#include "renderer/GpuProfiler.hpp"

#include "renderer/vulkan/Device.hpp"

namespace lumora::render {

GpuProfiler::GpuProfiler(const rhi::Device& device, std::uint32_t maxScopes)
    : device_(device), enabled_(device.caps().timestamps), maxQueries_(2 + maxScopes * 2),
      periodNs_(device.caps().timestampPeriodNs) {
    if (!enabled_) return;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = maxQueries_;
    VkQueryPool pool = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateQueryPool(device.handle(), &info, nullptr, &pool));
    pool_ = rhi::UniqueQueryPool(device.handle(), pool);
}

void GpuProfiler::beginFrame(VkCommandBuffer cmd) {
    if (!enabled_) return;
    names_.clear();
    vkCmdResetQueryPool(cmd, pool_.get(), 0, maxQueries_);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_.get(), 0);
    used_ = 2;
    recorded_ = true;
}

std::uint32_t GpuProfiler::beginScope(VkCommandBuffer cmd, const std::string& name) {
    if (!enabled_ || !recorded_ || used_ + 2 > maxQueries_) return UINT32_MAX;
    const std::uint32_t scope = static_cast<std::uint32_t>(names_.size());
    names_.push_back(name);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_.get(), used_);
    used_ += 2;
    return scope;
}

void GpuProfiler::endScope(VkCommandBuffer cmd, std::uint32_t scope) {
    if (!enabled_ || scope == UINT32_MAX) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_.get(), 3 + scope * 2);
}

void GpuProfiler::endFrame(VkCommandBuffer cmd) {
    if (!enabled_ || !recorded_) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_.get(), 1);
}

void GpuProfiler::resolve() {
    if (!enabled_ || !recorded_) return;
    recorded_ = false;
    std::vector<std::uint64_t> data(static_cast<std::size_t>(used_) * 2);  // value + availability
    const VkResult r = vkGetQueryPoolResults(device_.handle(), pool_.get(), 0, used_, data.size() * sizeof(std::uint64_t),
                                             data.data(), sizeof(std::uint64_t) * 2,
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return;
    auto value = [&](std::uint32_t q, std::uint64_t& out) {
        if (data[q * 2 + 1] == 0) return false;
        out = data[q * 2];
        return true;
    };
    auto toMs = [&](std::uint64_t begin, std::uint64_t end) {
        return end > begin ? static_cast<double>(end - begin) * periodNs_ * 1e-6 : 0.0;
    };
    results_.clear();
    std::uint64_t b = 0, e = 0;
    if (value(0, b) && value(1, e)) frameMs_ = toMs(b, e);
    for (std::uint32_t s = 0; s < names_.size(); ++s) {
        if (value(2 + s * 2, b) && value(3 + s * 2, e)) {
            results_.push_back({names_[s], toMs(b, e)});
        }
    }
}

} // namespace lumora::render
