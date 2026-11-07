#pragma once

#include "renderer/Gpu.hpp"
#include "renderer/TextureCompression.hpp"

#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lumora {
class AssetLibrary;
}

namespace lumora::render {

class RenderContext;

/// GPU textures for material maps, keyed by canonical (absolute) asset key and kind.
///
/// - Compression: colour maps become BC1 (BC3 with alpha, sRGB), data maps BC1, normal maps
///   BC5, with their whole mip chain, on the job system the first time; the result is kept in
///   <project>/Saved/TextureCache/ (keyed by the file, its size and time), so later runs upload
///   at once. Without BC support on the GPU, RGBA8.
/// - Streaming: callers say how many texels they need across (the size on screen); only the
///   mips down from that size are resident. A texture first appears at no more than 256 texels
///   (unless the caller blocks) and sharpens over the next frames, a few megabytes a frame.
///   Above the VRAM budget, unused textures are dropped and the rest lose their finest mips,
///   least needed first. Old GPU textures are released a few frames later (frames in flight).
class TextureCache {
public:
    static constexpr float kFullResolution = 1.0e9f;

    explicit TextureCache(RenderContext& context);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    /// Bindless index, or UINT32_MAX when the key is empty, cannot be loaded or (when not
    /// blocking) is still being prepared. `wanted`: texels across the caller needs.
    [[nodiscard]] std::uint32_t index(AssetLibrary& library, const std::string& key, TextureKind kind, bool block,
                                      float wanted = kFullResolution);
    /// Once per frame, before the frame's index() calls: streams mips in and out.
    void beginFrame();

    struct Stats {
        std::size_t textures = 0;       // with GPU memory
        std::size_t residentBytes = 0;
        std::size_t compressed = 0;     // of those, block-compressed
        std::size_t streamedIn = 0;     // textures that got finer mips last frame
        std::size_t reduced = 0;        // ... that lost mips or were dropped for the budget
    };
    [[nodiscard]] Stats stats() const;
    void setBudget(std::size_t bytes) noexcept { budget_ = bytes; }
    void setCompression(bool enabled) noexcept { compression_ = enabled; }
    [[nodiscard]] std::size_t size() const noexcept { return textures_.size(); }
    /// Lets textures that failed to load be tried again.
    void forgetFailures() { failed_.clear(); }
    /// Drops textures read from a file (absolute, generic form); the GPU must be idle.
    std::size_t invalidateFile(const std::string& file);

private:
    struct Entry {
        std::string name;
        std::shared_ptr<const TextureMips> mips;
        std::shared_future<std::shared_ptr<const TextureMips>> preparing;
        nvrhi::TextureHandle texture;
        std::uint32_t bindless = UINT32_MAX;
        std::uint32_t firstLevel = 0;  // resident levels: firstLevel .. the last
        float wanted = 0.0f;           // most texels asked for this frame
        std::uint64_t lastUsed = 0;
    };
    struct Retired {
        nvrhi::TextureHandle texture;
        std::uint32_t bindless = UINT32_MAX;
        std::uint64_t frame = 0;
    };
    [[nodiscard]] static std::uint32_t levelFor(const TextureMips& mips, float wanted);
    void upload(Entry& entry, std::uint32_t firstLevel);
    void retire(Entry& entry);

    RenderContext& ctx_;
    std::unordered_map<std::string, Entry> textures_;
    std::unordered_set<std::string> failed_;
    std::vector<Retired> retired_;
    std::uint64_t frame_ = 1;
    std::size_t budget_ = std::size_t{1536} << 20;
    std::size_t uploadPerFrame_ = std::size_t{48} << 20;
    bool compression_ = true;
    std::size_t streamedIn_ = 0;
    std::size_t reduced_ = 0;
};

} // namespace lumora::render
