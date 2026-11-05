#pragma once

#include "renderer/vulkan/VulkanCommon.hpp"

#include <glm/vec4.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lumora::rhi {
class Device;
}

namespace lumora::render {

class GpuProfiler;
class TransientImagePool;

/// How a pass touches an image. Each usage maps to one (layout, stages, access) state.
enum class ImageUsage : std::uint8_t {
    ColorAttachment,
    DepthAttachment,      // depth test + write
    DepthReadOnly,        // depth test, no write
    FragmentSampled,
    TransferSrc,
    TransferDst,
    Present,
};

struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    bool operator==(const ImageState&) const = default;
};

[[nodiscard]] ImageState stateForUsage(ImageUsage usage) noexcept;
[[nodiscard]] VkImageUsageFlags imageUsageFlags(ImageUsage usage) noexcept;
[[nodiscard]] bool hasWriteAccess(VkAccessFlags2 access) noexcept;
/// Barrier required between two states, or nullopt when the transition is a no-op
/// (read-after-read in the same layout). `discard` means the old contents are not needed.
[[nodiscard]] std::optional<VkImageMemoryBarrier2> makeImageBarrier(const ImageState& from, const ImageState& to,
                                                                    VkImage image, VkImageAspectFlags aspect,
                                                                    bool discard);

struct RGImage {
    std::uint32_t id = UINT32_MAX;
    [[nodiscard]] bool valid() const noexcept { return id != UINT32_MAX; }
    bool operator==(const RGImage&) const = default;
};

enum class LoadOp : std::uint8_t { Load, Clear, DontCare };

struct ImportedImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    ImageState initialState{};                    // state the image is in when the frame starts
    std::optional<ImageUsage> finalUsage;         // transition applied after the last pass
    std::uint32_t bindlessIndex = UINT32_MAX;
};

struct TransientImageDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

class RenderGraph;

/// Declares the resources a pass reads/writes; the graph derives barriers from this.
class PassBuilder {
public:
    void writeColor(RGImage image, LoadOp load, glm::vec4 clear = glm::vec4(0.0f));
    void writeDepth(RGImage image, LoadOp load, float clearDepth = 0.0f);
    void readDepth(RGImage image);
    void sample(RGImage image);
    void copyFrom(RGImage image);
    void copyTo(RGImage image);

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, std::uint32_t pass) : graph_(graph), pass_(pass) {}
    RenderGraph& graph_;
    std::uint32_t pass_;
};

/// Passed to pass callbacks during execution.
class PassContext {
public:
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    [[nodiscard]] VkImage image(RGImage h) const;
    [[nodiscard]] VkImageView view(RGImage h) const;
    [[nodiscard]] VkExtent2D extent(RGImage h) const;
    [[nodiscard]] std::uint32_t bindlessIndex(RGImage h) const;

private:
    friend class RenderGraph;
    explicit PassContext(const RenderGraph& graph) : graph_(graph) {}
    const RenderGraph& graph_;
};

/// Frame render graph: passes are declared each frame in execution order; the graph
/// realises transient images from a pool, computes synchronization2 barriers from the
/// declared usages, wraps raster passes in dynamic rendering and records GPU timings.
class RenderGraph {
public:
    using ExecuteFn = std::function<void(PassContext&)>;
    using SetupFn = std::function<void(PassBuilder&)>;

    RenderGraph() = default;

    RGImage importImage(std::string name, const ImportedImage& image);
    RGImage createImage(std::string name, const TransientImageDesc& desc);
    /// Raster passes run inside vkCmdBeginRendering over their declared attachments.
    void addRasterPass(std::string name, const SetupFn& setup, ExecuteFn execute);
    /// Transfer/compute passes run outside a rendering scope.
    void addPass(std::string name, const SetupFn& setup, ExecuteFn execute);

    struct PlannedBarrier {
        std::uint32_t pass;  // UINT32_MAX = after the last pass
        RGImage image;
        ImageState from;
        ImageState to;
        bool discard;
    };
    /// Pure barrier planning (no Vulkan calls) - exposed for testing and debugging.
    [[nodiscard]] std::vector<PlannedBarrier> planBarriers() const;
    /// Union of usage flags each transient image needs.
    [[nodiscard]] VkImageUsageFlags requiredUsage(RGImage image) const;

    /// Allocates transients, records all passes into `cmd`.
    void execute(VkCommandBuffer cmd, const rhi::Device& device, TransientImagePool& pool, GpuProfiler* profiler);

    [[nodiscard]] std::size_t passCount() const noexcept { return passes_.size(); }
    [[nodiscard]] const std::string& passName(std::size_t i) const { return passes_[i].name; }

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Access {
        RGImage image;
        ImageUsage usage;
        LoadOp load = LoadOp::Load;
        VkClearValue clear{};
    };
    struct Pass {
        std::string name;
        bool raster = false;
        std::vector<Access> accesses;
        ExecuteFn execute;
    };
    struct Resource {
        std::string name;
        bool imported = false;
        ImportedImage physical;  // filled at import or when realised
        TransientImageDesc desc;
    };

    void addAccess(std::uint32_t pass, const Access& access);

    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
};

} // namespace lumora::render
