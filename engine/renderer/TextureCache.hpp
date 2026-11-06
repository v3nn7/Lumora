#pragma once

#include "renderer/Gpu.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lumora {
class AssetLibrary;
}


namespace lumora::render {

class RenderContext;

enum class TextureColorSpace { Srgb, Linear };

/// GPU textures for material maps, keyed by canonical (absolute) asset key and colour space:
/// colour data (base colour, emissive) is sampled through sRGB views, data maps (normal,
/// metallic-roughness) through UNORM. Uploaded on first use with full mip chains and registered
/// in the bindless table. Entries live until the RenderContext is destroyed.
class TextureCache {
public:
    explicit TextureCache(RenderContext& context);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    /// Bindless index, or UINT32_MAX when the key is empty, cannot be loaded or (when not
    /// blocking) is still decoding in the background.
    [[nodiscard]] std::uint32_t index(AssetLibrary& library, const std::string& key, TextureColorSpace space, bool block);
    [[nodiscard]] std::size_t size() const noexcept { return textures_.size(); }
    /// Lets textures that failed to load be tried again.
    void forgetFailures() { failed_.clear(); }
    /// Drops textures read from a file (absolute, generic form); the GPU must be idle.
    std::size_t invalidateFile(const std::string& file);

private:
    struct Entry {
        nvrhi::TextureHandle texture;
        std::uint32_t bindless = UINT32_MAX;
    };
    RenderContext& ctx_;
    std::unordered_map<std::string, Entry> textures_;
    std::unordered_set<std::string> failed_;
};

} // namespace lumora::render
