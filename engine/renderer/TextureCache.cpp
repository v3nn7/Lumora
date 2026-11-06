#include "renderer/TextureCache.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "renderer/RenderContext.hpp"

#include <span>

namespace lumora::render {

TextureCache::TextureCache(RenderContext& context) : ctx_(context) {}

TextureCache::~TextureCache() {
    for (auto& [key, entry] : textures_) ctx_.bindless().releaseTexture(entry.bindless);
}

std::size_t TextureCache::invalidateFile(const std::string& file) {
    auto fromFile = [&](const std::string& id) {
        return id.starts_with(file) && id.size() > file.size() && (id[file.size()] == '|' || id[file.size()] == '#');
    };
    std::erase_if(failed_, fromFile);
    std::size_t dropped = 0;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (fromFile(it->first)) {
            if (it->second.bindless != UINT32_MAX) ctx_.bindless().releaseTexture(it->second.bindless);
            it = textures_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

std::uint32_t TextureCache::index(AssetLibrary& library, const std::string& key, TextureColorSpace space, bool block) {
    if (key.empty()) return UINT32_MAX;
    auto canonical = library.canonicalTextureKey(key);
    if (!canonical) return UINT32_MAX;
    const std::string id = *canonical + (space == TextureColorSpace::Srgb ? "|srgb" : "|linear");
    if (auto it = textures_.find(id); it != textures_.end()) return it->second.bindless;
    if (failed_.contains(id)) return UINT32_MAX;

    Stopwatch timer;
    const ImageData* data = nullptr;
    if (block) {
        auto loaded = library.texture(key);
        if (loaded) data = *loaded;
    } else {
        const auto request = library.requestTexture(key);
        if (request.state == AssetState::Pending) return UINT32_MAX;  // untextured until decoded
        data = request.value;
    }
    if (data == nullptr) {
        failed_.insert(id);
        return UINT32_MAX;
    }
    const bool srgb = space == TextureColorSpace::Srgb;
    const auto mips = buildMipChain(std::span(data->rgba), data->width, data->height, srgb);
    std::vector<std::span<const std::byte>> levels;
    for (const auto& level : mips) levels.push_back(std::as_bytes(std::span(level)));
    Entry entry;
    entry.texture = ctx_.createTexture(levels, data->width, data->height,
                                       srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM, key.c_str());
    entry.bindless = ctx_.bindless().registerTexture(entry.texture);
    log::debug("Renderer", "texture '{}' {}x{} ready in {:.1f} ms", key, data->width, data->height,
               timer.elapsedMs());
    return textures_.emplace(id, std::move(entry)).first->second.bindless;
}

} // namespace lumora::render
