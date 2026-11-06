#pragma once

#include "renderer/Gpu.hpp"

#include <glm/vec4.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lumora::render {

class GpuProfiler;
class TransientImagePool;

/// How a pass touches an image.
enum class ImageUsage : std::uint8_t {
    ColorAttachment,
    DepthAttachment,  // depth test + write
    DepthReadOnly,    // depth test, no write
    Sampled,          // read in shaders through its bindless index
    Storage,          // read and written by a compute pass (unordered access)
    CopySource,
    CopyDestination,
};

struct RGImage {
    std::uint32_t id = UINT32_MAX;
    [[nodiscard]] bool valid() const noexcept { return id != UINT32_MAX; }
    bool operator==(const RGImage&) const = default;
};

enum class LoadOp : std::uint8_t { Load, Clear, DontCare };

struct ImportedImage {
    nvrhi::TextureHandle texture;
    std::uint32_t bindlessIndex = UINT32_MAX;
};

struct TransientImageDesc {
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool storage = false;  // compute passes write it (unordered access)
};

class RenderGraph;

/// Declares the resources a pass reads/writes: the graph clears attachments, builds the
/// framebuffer and moves sampled images into shader-readable state from this.
class PassBuilder {
public:
    void writeColor(RGImage image, LoadOp load, glm::vec4 clear = glm::vec4(0.0f));
    void writeDepth(RGImage image, LoadOp load, float clearDepth = 0.0f);
    void readDepth(RGImage image);
    void sample(RGImage image);
    /// A compute pass reads and writes it (set 2 of the compute pipeline, in the order declared).
    void writeStorage(RGImage image);
    void copyFrom(RGImage image);
    void copyTo(RGImage image);
    /// The pass records its draws with PassContext::recordParallel, possibly into several
    /// command lists on the job system's threads.
    void recordInParallel(bool parallel = true);

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, std::uint32_t pass) : graph_(graph), pass_(pass) {}
    RenderGraph& graph_;
    std::uint32_t pass_;
};

/// Framebuffers for attachment sets, kept across frames (dropped when unused for a while).
class FramebufferCache {
public:
    nvrhi::IFramebuffer* get(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc, std::uint64_t frame);
    void prune(std::uint64_t frame);
    void clear() { entries_.clear(); }

private:
    struct Entry {
        nvrhi::FramebufferHandle framebuffer;
        std::uint64_t lastUsed = 0;
    };
    std::map<std::vector<const void*>, Entry> entries_;
};

/// What the graph needs from the renderer to run.
struct GraphRuntime {
    nvrhi::IDevice* device = nullptr;
    TransientImagePool* pool = nullptr;
    GpuProfiler* profiler = nullptr;
    FramebufferCache* framebuffers = nullptr;
    std::uint64_t frame = 0;
    /// Set 0 of every pipeline (frame constants, objects, lights, samplers) for this frame.
    nvrhi::IBindingSet* frameBindings = nullptr;
    /// Compute passes: the layout of their storage images (set 2) and a 1x1 image that fills
    /// the slots a pass leaves empty.
    nvrhi::IBindingLayout* storageLayout = nullptr;
    nvrhi::ITexture* storageFallback = nullptr;
    /// An open command list for work recorded after a parallel pass or on worker threads.
    std::function<nvrhi::CommandListHandle()> newList;
    bool allowParallel = true;
    std::uint32_t maxThreads = 8;
};

/// Passed to pass callbacks during execution.
class PassContext {
public:
    using ChunkFn = std::function<void(nvrhi::ICommandList* cmd, std::size_t begin, std::size_t end)>;

    nvrhi::ICommandList* cmd = nullptr;
    nvrhi::IFramebuffer* framebuffer = nullptr;  // raster passes

    [[nodiscard]] nvrhi::ITexture* texture(RGImage h) const;
    [[nodiscard]] std::uint32_t bindlessIndex(RGImage h) const;
    /// Viewport and scissor covering the framebuffer.
    [[nodiscard]] nvrhi::ViewportState viewport() const;
    /// Compute passes: the binding set of their storage images (set 2), in declaration order.
    [[nodiscard]] nvrhi::IBindingSet* storageBindings() const noexcept { return storageSet_; }
    /// Set 0 of this frame.
    [[nodiscard]] nvrhi::IBindingSet* frameBindings() const noexcept { return runtime_.frameBindings; }
    /// Records items [0, count) of a raster pass. In a pass declared with recordInParallel(),
    /// chunks of at least `grain` items are recorded into separate command lists on the job
    /// system's threads and executed in order; otherwise fn(cmd, 0, count) records inline.
    /// fn must set its whole graphics state (each list starts empty) and only read shared data.
    void recordParallel(std::size_t count, std::size_t grain, const ChunkFn& fn);
    /// Number of command lists the last recordParallel used (0 = inline).
    [[nodiscard]] std::uint32_t lastChunkCount() const noexcept { return lastChunks_; }

private:
    friend class RenderGraph;
    PassContext(RenderGraph& graph, GraphRuntime& runtime, std::uint32_t pass)
        : graph_(graph), runtime_(runtime), pass_(pass) {}
    void prepareSampled(nvrhi::ICommandList* list) const;
    RenderGraph& graph_;
    GraphRuntime& runtime_;
    std::uint32_t pass_;
    std::uint32_t lastChunks_ = 0;
    nvrhi::IBindingSet* storageSet_ = nullptr;
};

/// Frame render graph: passes are declared each frame in execution order; the graph realises
/// transient images from a pool, clears attachments, builds framebuffers, puts sampled images
/// into shader-readable state, times each pass and splits parallel passes over command lists.
class RenderGraph {
public:
    using ExecuteFn = std::function<void(PassContext&)>;
    using SetupFn = std::function<void(PassBuilder&)>;

    RenderGraph() = default;

    RGImage importImage(std::string name, const ImportedImage& image);
    RGImage createImage(std::string name, const TransientImageDesc& desc);

    /// Raster passes draw into the framebuffer of their declared attachments.
    void addRasterPass(std::string name, const SetupFn& setup, ExecuteFn execute);
    /// Transfer passes (copies) record outside a framebuffer.
    void addPass(std::string name, const SetupFn& setup, ExecuteFn execute);
    /// Compute passes dispatch outside a framebuffer; PassContext::storageBindings() holds the
    /// images they declared with writeStorage.
    void addComputePass(std::string name, const SetupFn& setup, ExecuteFn execute);
    /// Storage image slots in a compute pass's set 2.
    static constexpr std::uint32_t kStorageSlots = 4;

    /// Records every pass, starting in `first` (open). Returns the command lists in execution
    /// order: all closed except the last one, which stays open for the caller.
    std::vector<nvrhi::CommandListHandle> execute(nvrhi::CommandListHandle first, GraphRuntime& runtime);

    /// Command lists recorded by parallel passes in the last execute().
    [[nodiscard]] std::uint32_t parallelChunksRecorded() const noexcept { return parallelChunks_; }
    [[nodiscard]] std::size_t passCount() const noexcept { return passes_.size(); }
    [[nodiscard]] const std::string& passName(std::size_t i) const { return passes_[i].name; }
    [[nodiscard]] std::size_t imageCount() const noexcept { return resources_.size(); }

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Access {
        RGImage image;
        ImageUsage usage;
        LoadOp load = LoadOp::Load;
        glm::vec4 clear{0.0f};
    };
    struct Pass {
        std::string name;
        bool raster = false;
        bool parallel = false;
        bool compute = false;
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
    [[nodiscard]] const Resource& resource(RGImage h) const;

    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
    std::vector<nvrhi::CommandListHandle> lists_;  // during execute
    std::vector<nvrhi::FramebufferHandle> ownedFramebuffers_;
    std::vector<nvrhi::BindingSetHandle> ownedBindingSets_;
    std::uint32_t parallelChunks_ = 0;
};

} // namespace lumora::render
