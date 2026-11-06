#include "renderer/LightProbes.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/FileSystem.hpp"
#include "core/Log.hpp"
#include "renderer/EnvironmentMap.hpp"
#include "renderer/ReflectionProbes.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Camera.hpp"
#include "scene/Components.hpp"
#include "scene/Scene.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

namespace lumora::render {

namespace {

constexpr char kMagic[4] = {'L', 'P', 'V', '1'};
constexpr std::uint32_t kFaceSize = 16;  // pixels per cube face of a probe

std::string newKey(const Scene& scene, entt::entity e, int bounce) {
    return std::format("Saved/Probes/{}-{:x}-{}.lpv", scene.uuidOf(e).toString(),
                       static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()), bounce);
}

} // namespace

glm::uvec3 probeVolumeCounts(const glm::vec3& size, float spacing) {
    spacing = std::max(spacing, 0.05f);
    for (;;) {
        glm::uvec3 n;
        for (int a = 0; a < 3; ++a) {
            n[a] = static_cast<std::uint32_t>(std::clamp(std::ceil(std::max(size[a], 0.0f) / spacing), 1.0f, 64.0f));
        }
        if (n.x * n.y * n.z <= 8192u) return n;
        spacing *= 1.25f;
    }
}

Result<> saveProbeVolume(const std::filesystem::path& file, const ProbeVolumeData& data) {
    const std::size_t probes = static_cast<std::size_t>(data.counts.x) * data.counts.y * data.counts.z;
    if (probes == 0 || data.sh.size() != probes * 9) return makeError("{}: invalid probe volume", file.filename().string());
    std::vector<std::byte> bytes(4 + 12 + data.sh.size() * 12);
    std::memcpy(bytes.data(), kMagic, 4);
    std::memcpy(bytes.data() + 4, &data.counts, 12);
    std::memcpy(bytes.data() + 16, data.sh.data(), data.sh.size() * 12);
    if (std::error_code ec; !std::filesystem::create_directories(file.parent_path(), ec) && ec) {
        return makeError("{}: {}", file.parent_path().string(), ec.message());
    }
    return fs::writeFileAtomic(file, bytes);
}

Result<ProbeVolumeData> loadProbeVolume(const std::filesystem::path& file) {
    auto bytes = fs::readBinaryFile(file);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() < 16 || std::memcmp(bytes->data(), kMagic, 4) != 0) {
        return makeError("{}: not a light probe volume", file.filename().string());
    }
    ProbeVolumeData data;
    std::memcpy(&data.counts, bytes->data() + 4, 12);
    const std::uint64_t probes = static_cast<std::uint64_t>(data.counts.x) * data.counts.y * data.counts.z;
    if (glm::any(glm::equal(data.counts, glm::uvec3(0u))) || glm::any(glm::greaterThan(data.counts, glm::uvec3(64u))) ||
        bytes->size() != 16 + probes * 9 * 12) {
        return makeError("{}: damaged light probe volume", file.filename().string());
    }
    data.sh.resize(probes * 9);
    std::memcpy(data.sh.data(), bytes->data() + 16, data.sh.size() * 12);
    return data;
}

Result<int> bakeLightProbeVolumes(Renderer& renderer, Scene& scene, AssetLibrary& assets, RenderSettings settings,
                                  bool keepPrevious, const std::function<bool(float)>& progress) {
    settings.temporalAntiAliasing = false;
    settings.screenSpaceReflections = false;
    settings.virtualShadows = false;  // cascades: the camera jumps from probe to probe
    settings.ambientOcclusion = false;
    settings.bloom = false;
    settings.showGrid = false;
    settings.antiAliasing = false;
    settings.overlay = nullptr;
    settings.sprites = nullptr;
    settings.hud = nullptr;
    settings.debug = nullptr;
    settings.selected = entt::null;
    settings.alsoSelected.clear();
    settings.assets = &assets;
    settings.blockOnAssets = true;
    scene.updateWorldTransforms();
    auto& reg = scene.registry();

    struct Volume {
        entt::entity entity;
        glm::vec3 boxMin;
        glm::vec3 size;
        glm::uvec3 counts;
        int bounces;
        std::string original;      // its file before this bake
        std::vector<std::string> written;
    };
    std::vector<Volume> volumes;
    std::uint64_t work = 0;
    for (auto [e, volume, world] : reg.view<LightProbeVolumeComponent, WorldTransformComponent>().each()) {
        const glm::vec3 size = glm::max(volume.size, glm::vec3(0.01f));
        const glm::uvec3 counts = probeVolumeCounts(size, volume.spacing);
        const int bounces = std::clamp(volume.bounces, 1, 8);
        volumes.push_back({e, glm::vec3(world.matrix[3]) - size * 0.5f, size, counts, bounces, volume.baked, {}});
        work += static_cast<std::uint64_t>(counts.x) * counts.y * counts.z * static_cast<std::uint64_t>(bounces);
    }
    if (volumes.empty()) return 0;
    int maxBounces = 0;
    for (const Volume& v : volumes) maxBounces = std::max(maxBounces, v.bounces);

    // Removes this bake's files (but the last of each volume when keepLast); on failure the
    // volumes point at their old files again.
    auto cleanUp = [&](bool keepLast) {
        for (Volume& v : volumes) {
            if (!keepLast) reg.get<LightProbeVolumeComponent>(v.entity).baked = v.original;
            for (std::size_t i = 0; i < v.written.size(); ++i) {
                if (keepLast && i + 1 == v.written.size()) continue;
                std::error_code ec;
                std::filesystem::remove(assets.root() / v.written[i], ec);
            }
        }
    };
    std::uint64_t done = 0;
    for (int bounce = 0; bounce < maxBounces; ++bounce) {
        // The first bounce sees direct light and the sky; later ones also the volumes so far.
        settings.lightProbeVolumes = bounce > 0;
        std::vector<ProbeVolumeData> results(volumes.size());
        for (std::size_t vi = 0; vi < volumes.size(); ++vi) {
            const Volume& v = volumes[vi];
            if (bounce >= v.bounces) continue;
            ProbeVolumeData& data = results[vi];
            data.counts = v.counts;
            data.sh.resize(static_cast<std::size_t>(v.counts.x) * v.counts.y * v.counts.z * 9);
            const glm::vec3 cell = v.size / glm::vec3(v.counts);
            for (std::uint32_t z = 0; z < v.counts.z; ++z) {
                for (std::uint32_t y = 0; y < v.counts.y; ++y) {
                    for (std::uint32_t x = 0; x < v.counts.x; ++x) {
                        const glm::vec3 position = v.boxMin + (glm::vec3(x, y, z) + 0.5f) * cell;
                        std::array<HdrImage, 6> faces;
                        for (std::size_t f = 0; f < 6; ++f) {
                            const CubeFace& face = cubeFaces()[f];
                            glm::mat4 cameraWorld(1.0f);
                            cameraWorld[0] = glm::vec4(glm::cross(face.forward, face.up), 0.0f);
                            cameraWorld[1] = glm::vec4(face.up, 0.0f);
                            cameraWorld[2] = glm::vec4(-face.forward, 0.0f);
                            cameraWorld[3] = glm::vec4(position, 1.0f);
                            auto image = renderer.captureHdr(scene, makeCameraView(cameraWorld, 90.0f, 0.02f, 1.0f),
                                                             {kFaceSize, kFaceSize}, settings);
                            if (!image) {
                                cleanUp(false);
                                return std::unexpected(image.error());
                            }
                            faces[f] = std::move(*image);
                        }
                        const auto sh = projectIrradiance(cubeToEquirect(faces, kFaceSize * 4));
                        std::copy(sh.begin(), sh.end(), data.sh.begin() + static_cast<std::ptrdiff_t>(((z * v.counts.y + y) * v.counts.x + x) * 9));
                        ++done;
                        if (progress && !progress(static_cast<float>(done) / static_cast<float>(work))) {
                            cleanUp(false);
                            return makeError("baking light probes was cancelled");
                        }
                    }
                }
            }
        }
        // Written after the bounce, so every volume of this bounce saw the same previous light.
        for (std::size_t vi = 0; vi < volumes.size(); ++vi) {
            Volume& v = volumes[vi];
            if (bounce >= v.bounces) continue;
            const std::string key = newKey(scene, v.entity, bounce);
            if (auto saved = saveProbeVolume(assets.root() / key, results[vi]); !saved) {
                cleanUp(false);
                return std::unexpected(saved.error());
            }
            v.written.push_back(key);
            reg.get<LightProbeVolumeComponent>(v.entity).baked = key;
        }
    }
    cleanUp(true);  // the bounces in between
    if (!keepPrevious) {
        for (const Volume& v : volumes) {
            if (v.original.starts_with("Saved/Probes/")) {
                std::error_code ec;
                std::filesystem::remove(assets.root() / v.original, ec);
            }
        }
    }
    log::info("Renderer", "baked {} light probe volume(s)", volumes.size());
    return static_cast<int>(volumes.size());
}

} // namespace lumora::render
