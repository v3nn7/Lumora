#pragma once

#include "renderer/vulkan/Resources.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lumora {
class AssetLibrary;
}

namespace lumora::rhi {
class BindlessTable;
class UploadQueue;
}

namespace lumora::render {

struct EnvironmentMap;

/// GPU image-based lighting for a scene's sky texture (an equirectangular .hdr): the sky image
/// itself, its GGX-prefiltered specular mip chain and SH9 irradiance. Loading and prefiltering
/// run on the job system; until they finish the scene keeps the procedural sky. Entries live as
/// long as the RenderContext.
class EnvironmentCache {
public:
    struct Gpu {
        rhi::Image sky;
        rhi::Image specular;
        std::uint32_t skyIndex = UINT32_MAX;
        std::uint32_t specularIndex = UINT32_MAX;
        std::uint32_t specularLevels = 0;
        std::array<glm::vec3, 9> irradiance{};
    };

    EnvironmentCache(rhi::UploadQueue& uploads, rhi::BindlessTable& bindless);
    ~EnvironmentCache();
    EnvironmentCache(const EnvironmentCache&) = delete;
    EnvironmentCache& operator=(const EnvironmentCache&) = delete;

    /// The environment for `key`, or null while it loads (block = wait for it) or if it failed.
    [[nodiscard]] const Gpu* get(AssetLibrary& library, const std::string& key, bool block);

private:
    struct Prepared;
    struct Entry {
        std::shared_future<std::shared_ptr<Prepared>> loading;
        std::unique_ptr<Gpu> gpu;
        bool failed = false;
    };
    rhi::UploadQueue& uploads_;
    rhi::BindlessTable& bindless_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_set<std::string> warned_;
};

} // namespace lumora::render
