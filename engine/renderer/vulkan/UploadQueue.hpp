#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <span>

namespace lumora::rhi {

class Device;

/// Non-blocking resource uploads. Each upload records a staging copy into its own command
/// buffer and signals a timeline semaphore; frame submissions wait on the latest value
/// (GPU-side), so the CPU never stalls on transfers. Staging memory is reclaimed once the
/// timeline passes the upload's value.
///
/// Uses the graphics queue; a dedicated transfer queue (with ownership transfers) is a
/// planned optimisation once asset streaming lands.
class UploadQueue {
public:
    explicit UploadQueue(Device& device);
    ~UploadQueue();
    UploadQueue(const UploadQueue&) = delete;
    UploadQueue& operator=(const UploadQueue&) = delete;

    /// Creates a device-local buffer with `usage | TRANSFER_DST` and schedules the copy.
    [[nodiscard]] Buffer uploadBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage, const char* name);
    /// Creates a sampled 2D image from tightly packed RGBA8 pixels, generating the full mip
    /// chain with linear blits when the format supports it. The image ends up in
    /// SHADER_READ_ONLY_OPTIMAL once the returned upload completes.
    [[nodiscard]] Image uploadImage(std::span<const std::byte> rgba, VkExtent2D extent, VkFormat format, bool mipmaps,
                                    const char* name);

    [[nodiscard]] VkSemaphore timeline() const noexcept { return timeline_.get(); }
    /// Highest value signalled by a submitted upload (0 if none).
    [[nodiscard]] std::uint64_t lastSubmittedValue() const noexcept { return lastSubmitted_; }
    /// Frees staging buffers of completed uploads.
    void collect();
    /// Blocks until all uploads finished (shutdown / tests).
    void waitIdle();

private:
    VkCommandBuffer beginCommands();
    void submit(VkCommandBuffer cmd, Buffer staging);

    struct Pending {
        std::uint64_t value;
        Buffer staging;
        VkCommandBuffer cmd;
    };

    Device& device_;
    UniqueCommandPool pool_;
    UniqueSemaphore timeline_;
    std::uint64_t lastSubmitted_ = 0;
    std::deque<Pending> pending_;
    std::mutex mutex_;
};

} // namespace lumora::rhi
