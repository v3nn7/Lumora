#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <cstdint>
#include <vector>

namespace lumora::rhi {
class Device;
}

namespace lumora::render {

/// Command pools for recording one pass on several threads: chunk c of a pass records into a
/// secondary command buffer from pool c, so no pool is ever used by two threads at once (passes
/// run one after another). One set per frame in flight; reset once that frame's fence has
/// signalled, which frees every secondary allocated from it.
class ParallelCommandPools {
public:
    ParallelCommandPools(const rhi::Device& device, std::uint32_t chunks);

    [[nodiscard]] std::uint32_t chunks() const noexcept { return static_cast<std::uint32_t>(pools_.size()); }
    void reset();
    /// A new secondary command buffer from chunk `chunk`'s pool. Only that chunk's thread may call this.
    [[nodiscard]] VkCommandBuffer allocate(std::uint32_t chunk);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<rhi::UniqueCommandPool> pools_;
};

} // namespace lumora::render
