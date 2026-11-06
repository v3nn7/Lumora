#include "renderer/EnvironmentCache.hpp"

#include "assets/AssetLibrary.hpp"
#include "assets/Image.hpp"
#include "core/JobSystem.hpp"
#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "renderer/EnvironmentMap.hpp"
#include "renderer/RenderContext.hpp"

#include <glm/gtc/packing.hpp>

#include <chrono>
#include <vector>

namespace lumora::render {

/// CPU result of loading and prefiltering, handed from the job to the render thread.
struct EnvironmentCache::Prepared {
    std::string error;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint16_t> sky;  // RGBA16F
    std::vector<std::vector<std::uint16_t>> specular;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> specularExtents;
    std::array<glm::vec3, 9> irradiance{};
};

namespace {

std::vector<std::uint16_t> toHalfRgba(const std::vector<float>& rgb, std::size_t texels) {
    std::vector<std::uint16_t> out(texels * 4);
    const std::uint16_t one = glm::packHalf1x16(1.0f);
    for (std::size_t i = 0; i < texels; ++i) {
        // Clamp to the half range: a sun stored as 1e6 would otherwise become infinity.
        out[i * 4 + 0] = glm::packHalf1x16(std::min(rgb[i * 3 + 0], 60000.0f));
        out[i * 4 + 1] = glm::packHalf1x16(std::min(rgb[i * 3 + 1], 60000.0f));
        out[i * 4 + 2] = glm::packHalf1x16(std::min(rgb[i * 3 + 2], 60000.0f));
        out[i * 4 + 3] = one;
    }
    return out;
}

} // namespace

EnvironmentCache::EnvironmentCache(RenderContext& context) : ctx_(context) {}

EnvironmentCache::~EnvironmentCache() {
    for (auto& [key, entry] : entries_) {
        if (entry.loading.valid()) entry.loading.wait();  // jobs reference nothing of ours, but finish cleanly
        if (entry.gpu) {
            ctx_.bindless().releaseTexture(entry.gpu->skyIndex);
            ctx_.bindless().releaseTexture(entry.gpu->specularIndex);
        }
    }
}

const EnvironmentCache::Gpu* EnvironmentCache::get(AssetLibrary& library, const std::string& key, bool block) {
    if (key.empty()) return nullptr;
    // Keyed by absolute path: the cache outlives projects.
    auto file = library.filePath(key);
    if (!file) {
        if (warned_.insert(key).second) log::warn("Renderer", "sky texture '{}': {}", key, file.error().message);
        return nullptr;
    }
    Entry& entry = entries_[file->generic_string()];
    if (entry.gpu) return entry.gpu.get();
    if (entry.failed) return nullptr;
    if (!entry.loading.valid()) {
        entry.loading = JobSystem::global()
                            .submit([path = *file]() {
                                auto prepared = std::make_shared<Prepared>();
                                Stopwatch timer;
                                auto hdr = loadHdrFile(path);
                                if (!hdr) {
                                    prepared->error = hdr.error().message;
                                    return prepared;
                                }
                                const EnvironmentMap env = prefilterEnvironment(*hdr);
                                prepared->width = hdr->width;
                                prepared->height = hdr->height;
                                prepared->sky = toHalfRgba(hdr->rgb, static_cast<std::size_t>(hdr->width) * hdr->height);
                                for (const auto& level : env.specular) {
                                    prepared->specular.push_back(
                                        toHalfRgba(level.rgb, static_cast<std::size_t>(level.width) * level.height));
                                    prepared->specularExtents.push_back({level.width, level.height});
                                }
                                prepared->irradiance = env.irradiance;
                                log::info("Renderer", "environment {} ({}x{}) prefiltered in {:.0f} ms",
                                          path.filename().string(), hdr->width, hdr->height, timer.elapsedMs());
                                return prepared;
                            })
                            .share();
    }
    if (!block && entry.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    const std::shared_ptr<Prepared> prepared = entry.loading.get();
    entry.loading = {};
    if (!prepared->error.empty()) {
        log::warn("Renderer", "sky texture '{}': {}", key, prepared->error);
        entry.failed = true;
        return nullptr;
    }
    auto gpu = std::make_unique<Gpu>();
    const nvrhi::Format format = nvrhi::Format::RGBA16_FLOAT;
    const std::span<const std::byte> skyBytes = std::as_bytes(std::span(prepared->sky));
    gpu->sky = ctx_.createTexture(std::span(&skyBytes, 1), prepared->width, prepared->height, format, "EnvironmentSky");
    std::vector<std::span<const std::byte>> levels;
    for (const auto& level : prepared->specular) levels.push_back(std::as_bytes(std::span(level)));
    gpu->specular = ctx_.createTexture(levels, prepared->specularExtents.front().first,
                                       prepared->specularExtents.front().second, format, "EnvironmentSpecular");
    gpu->skyIndex = ctx_.bindless().registerTexture(gpu->sky);
    gpu->specularIndex = ctx_.bindless().registerTexture(gpu->specular);
    gpu->specularLevels = static_cast<std::uint32_t>(levels.size());
    gpu->irradiance = prepared->irradiance;
    entry.gpu = std::move(gpu);
    return entry.gpu.get();
}

} // namespace lumora::render
