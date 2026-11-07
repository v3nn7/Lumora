#include "renderer/TextureCache.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/JobSystem.hpp"
#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "renderer/RenderContext.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>

namespace lumora::render {

namespace {

constexpr std::uint64_t kRetireFrames = 4;       // frames in flight, and then some
constexpr std::uint32_t kFirstAppearance = 256;  // texels across a texture shows at first
constexpr std::uint64_t kUnusedFrames = 120;     // then it may be dropped for the budget
constexpr std::uint32_t kCacheVersion = 1;       // bump when prepared textures change

/// <root>/Saved/TextureCache/<hash>.ltex for a texture read from `file`, or empty.
std::filesystem::path cacheFile(const std::filesystem::path& root, const std::string& id, const std::string& file) {
    if (root.empty()) return {};
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) return {};
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec) return {};
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const char c : id) mix(static_cast<unsigned char>(c));
    mix(size);
    mix(static_cast<std::uint64_t>(time.time_since_epoch().count()));
    mix(kCacheVersion);
    return root / "Saved" / "TextureCache" / std::format("{:016x}.ltex", h);
}

} // namespace

TextureCache::TextureCache(RenderContext& context) : ctx_(context) {
    compression_ = ctx_.caps().textureCompressionBC;
}

TextureCache::~TextureCache() {
    for (auto& [key, entry] : textures_) {
        if (entry.preparing.valid()) entry.preparing.wait();
        ctx_.bindless().releaseTexture(entry.bindless);
    }
    for (const Retired& r : retired_) ctx_.bindless().releaseTexture(r.bindless);
}

std::size_t TextureCache::invalidateFile(const std::string& file) {
    auto fromFile = [&](const std::string& id) {
        return id.starts_with(file) && id.size() > file.size() && (id[file.size()] == '|' || id[file.size()] == '#');
    };
    std::erase_if(failed_, fromFile);
    std::size_t dropped = 0;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (fromFile(it->first)) {
            if (it->second.preparing.valid()) it->second.preparing.wait();
            ctx_.bindless().releaseTexture(it->second.bindless);  // the GPU is idle
            it = textures_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

std::uint32_t TextureCache::levelFor(const TextureMips& mips, float wanted) {
    const float largest = static_cast<float>(std::max(mips.width, mips.height));
    if (!(wanted > 0.0f) || wanted >= largest) return 0;
    const auto level = static_cast<std::uint32_t>(std::floor(std::log2(largest / std::max(wanted, 1.0f))));
    return std::min(level, static_cast<std::uint32_t>(mips.levels.size() - 1));
}

void TextureCache::retire(Entry& entry) {
    if (entry.texture) retired_.push_back({entry.texture, entry.bindless, frame_});
    entry.texture = nullptr;
    entry.bindless = UINT32_MAX;
}

void TextureCache::upload(Entry& entry, std::uint32_t firstLevel) {
    const TextureMips& mips = *entry.mips;
    std::vector<std::span<const std::byte>> levels;
    for (std::size_t l = firstLevel; l < mips.levels.size(); ++l) levels.push_back(std::as_bytes(std::span(mips.levels[l])));
    nvrhi::TextureHandle texture =
        ctx_.createTexture(levels, mips.levelWidth(firstLevel), mips.levelHeight(firstLevel), mips.format, entry.name.c_str());
    retire(entry);
    entry.texture = texture;
    entry.bindless = ctx_.bindless().registerTexture(texture);
    entry.firstLevel = firstLevel;
}

std::uint32_t TextureCache::index(AssetLibrary& library, const std::string& key, TextureKind kind, bool block, float wanted) {
    if (key.empty()) return UINT32_MAX;
    auto canonical = library.canonicalTextureKey(key);
    if (!canonical) return UINT32_MAX;
    static constexpr const char* kSuffix[] = {"|color", "|data", "|normal"};
    const std::string id = *canonical + kSuffix[static_cast<int>(kind)];
    if (failed_.contains(id)) return UINT32_MAX;
    Entry& entry = textures_[id];
    entry.lastUsed = frame_;
    entry.wanted = std::max(entry.wanted, wanted);
    if (entry.texture) return entry.bindless;

    if (!entry.mips) {
        if (!entry.preparing.valid()) {
            entry.name = key;
            const std::string file = canonical->substr(0, canonical->find('#'));
            const std::filesystem::path cached = cacheFile(library.root(), id, file);
            if (!cached.empty() && std::filesystem::exists(cached)) {
                if (auto loaded = loadTextureMips(cached)) {
                    entry.mips = std::make_shared<const TextureMips>(std::move(*loaded));
                } else {
                    log::warn("Renderer", "texture cache {}: {}", cached.filename().string(), loaded.error().message);
                }
            }
            if (!entry.mips) {
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
                    textures_.erase(id);
                    failed_.insert(id);
                    return UINT32_MAX;
                }
                const bool compress = compression_;
                entry.preparing =
                    JobSystem::global()
                        .submit([rgba = data->rgba, width = data->width, height = data->height, kind, compress, cached,
                                 name = key]() -> std::shared_ptr<const TextureMips> {
                            Stopwatch timer;
                            auto mips = std::make_shared<TextureMips>(prepareTexture(rgba, width, height, kind, compress));
                            if (!cached.empty() && compress) {
                                if (auto saved = saveTextureMips(cached, *mips); !saved) {
                                    log::warn("Renderer", "texture cache for '{}': {}", name, saved.error().message);
                                }
                            }
                            log::debug("Renderer", "texture '{}' {}x{} prepared in {:.1f} ms", name, width, height, timer.elapsedMs());
                            return mips;
                        })
                        .share();
            }
        }
        if (!entry.mips) {
            if (!block && entry.preparing.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return UINT32_MAX;
            entry.mips = entry.preparing.get();
            entry.preparing = {};
        }
    }
    std::uint32_t first = levelFor(*entry.mips, entry.wanted);
    if (!block) first = std::max(first, levelFor(*entry.mips, static_cast<float>(kFirstAppearance)));
    upload(entry, first);
    return entry.bindless;
}

void TextureCache::beginFrame() {
    ++frame_;
    // GPU textures replaced a few frames ago are no longer in use.
    std::erase_if(retired_, [&](const Retired& r) {
        if (r.frame + kRetireFrames > frame_) return false;
        ctx_.bindless().releaseTexture(r.bindless);
        return true;
    });
    // What each texture should have: the finest level anyone asked for last frame. Finer first
    // for the most needed (largest step), within the upload budget of a frame.
    struct Want {
        Entry* entry;
        std::uint32_t level;
    };
    std::vector<Want> finer;
    std::size_t resident = 0;
    for (auto& [id, entry] : textures_) {
        if (!entry.texture || !entry.mips) {
            entry.wanted = 0.0f;
            continue;
        }
        resident += entry.mips->bytes(entry.firstLevel);
        if (entry.lastUsed + 1 >= frame_) {
            const std::uint32_t level = levelFor(*entry.mips, entry.wanted);
            if (level < entry.firstLevel) finer.push_back({&entry, level});
        }
        entry.wanted = 0.0f;
    }
    std::ranges::sort(finer, [](const Want& a, const Want& b) {
        return a.entry->firstLevel - a.level > b.entry->firstLevel - b.level;
    });
    streamedIn_ = 0;
    reduced_ = 0;
    std::size_t uploaded = 0;
    for (const Want& w : finer) {
        const std::size_t bytes = w.entry->mips->bytes(w.level);
        if (uploaded > 0 && uploaded + bytes > uploadPerFrame_) break;
        const std::size_t extra = bytes - w.entry->mips->bytes(w.entry->firstLevel);
        if (resident + extra > budget_) continue;  // would only be taken away again
        resident += extra;
        upload(*w.entry, w.level);
        uploaded += bytes;
        ++streamedIn_;
    }
    if (resident <= budget_) return;
    // Over budget: drop what nothing used for a while (oldest first), then take the finest level
    // off the largest textures in use until it fits.
    std::vector<Entry*> unused;
    std::vector<Entry*> used;
    for (auto& [id, entry] : textures_) {
        if (!entry.texture) continue;
        (entry.lastUsed + kUnusedFrames < frame_ ? unused : used).push_back(&entry);
    }
    std::ranges::sort(unused, std::less<>{}, &Entry::lastUsed);
    for (Entry* e : unused) {
        if (resident <= budget_) return;
        resident -= e->mips->bytes(e->firstLevel);
        retire(*e);
        ++reduced_;
    }
    std::ranges::sort(used, [](const Entry* a, const Entry* b) { return a->mips->bytes(a->firstLevel) > b->mips->bytes(b->firstLevel); });
    for (Entry* e : used) {
        if (resident <= budget_) return;
        if (e->firstLevel + 1 >= e->mips->levels.size()) continue;
        resident -= e->mips->bytes(e->firstLevel) - e->mips->bytes(e->firstLevel + 1);
        upload(*e, e->firstLevel + 1);
        ++reduced_;
    }
}

TextureCache::Stats TextureCache::stats() const {
    Stats s;
    for (const auto& [id, entry] : textures_) {
        if (!entry.texture || !entry.mips) continue;
        ++s.textures;
        s.residentBytes += entry.mips->bytes(entry.firstLevel);
        if (entry.mips->format != nvrhi::Format::RGBA8_UNORM && entry.mips->format != nvrhi::Format::SRGBA8_UNORM) ++s.compressed;
    }
    s.streamedIn = streamedIn_;
    s.reduced = reduced_;
    return s;
}

} // namespace lumora::render
