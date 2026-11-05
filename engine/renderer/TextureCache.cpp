#include "renderer/TextureCache.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "renderer/vulkan/BindlessTable.hpp"
#include "renderer/vulkan/UploadQueue.hpp"

#include <span>

namespace lumora::render {

TextureCache::TextureCache(rhi::UploadQueue& uploads, rhi::BindlessTable& bindless)
    : uploads_(uploads), bindless_(bindless) {}

TextureCache::~TextureCache() {
    for (auto& [key, entry] : textures_) bindless_.releaseImage(entry.bindless);
}

std::uint32_t TextureCache::index(AssetLibrary& library, const std::string& key, TextureColorSpace space) {
    if (key.empty()) return UINT32_MAX;
    auto canonical = library.canonicalTextureKey(key);
    if (!canonical) return UINT32_MAX;
    const std::string id = *canonical + (space == TextureColorSpace::Srgb ? "|srgb" : "|linear");
    if (auto it = textures_.find(id); it != textures_.end()) return it->second.bindless;
    if (failed_.contains(id)) return UINT32_MAX;

    Stopwatch timer;
    auto image = library.texture(key);
    if (!image) {
        failed_.insert(id);
        return UINT32_MAX;
    }
    const VkFormat format = space == TextureColorSpace::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    Entry entry;
    entry.image = uploads_.uploadImage(std::as_bytes(std::span((*image)->rgba)), {(*image)->width, (*image)->height},
                                       format, true, key.c_str());
    entry.bindless = bindless_.registerImage(entry.image.view());
    log::debug("Renderer", "texture '{}' {}x{} ready in {:.1f} ms", key, (*image)->width, (*image)->height,
               timer.elapsedMs());
    return textures_.emplace(id, std::move(entry)).first->second.bindless;
}

} // namespace lumora::render
