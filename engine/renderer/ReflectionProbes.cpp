#include "renderer/ReflectionProbes.hpp"

#include "assets/AssetLibrary.hpp"
#include "core/Log.hpp"
#include "renderer/EnvironmentMap.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Camera.hpp"
#include "scene/Components.hpp"
#include "scene/Scene.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace lumora::render {

const std::array<CubeFace, 6>& cubeFaces() {
    static const std::array<CubeFace, 6> faces = {{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    }};
    return faces;
}

namespace {

glm::vec3 sampleFace(const HdrImage& face, float x, float y) {  // x, y in [-1, 1]; y up
    const float fx = std::clamp((x * 0.5f + 0.5f) * static_cast<float>(face.width) - 0.5f, 0.0f, static_cast<float>(face.width - 1));
    const float fy = std::clamp((0.5f - y * 0.5f) * static_cast<float>(face.height) - 0.5f, 0.0f, static_cast<float>(face.height - 1));
    const auto x0 = static_cast<std::uint32_t>(fx), y0 = static_cast<std::uint32_t>(fy);
    const std::uint32_t x1 = std::min(x0 + 1, face.width - 1), y1 = std::min(y0 + 1, face.height - 1);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    auto at = [&](std::uint32_t px, std::uint32_t py) {
        const float* p = &face.rgb[(static_cast<std::size_t>(py) * face.width + px) * 3];
        return glm::vec3(p[0], p[1], p[2]);
    };
    return glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx), glm::mix(at(x0, y1), at(x1, y1), tx), ty);
}

} // namespace

HdrImage cubeToEquirect(const std::array<HdrImage, 6>& faces, std::uint32_t width) {
    HdrImage out;
    out.width = std::max(width, 2u);
    out.height = out.width / 2;
    out.rgb.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    const auto& basis = cubeFaces();
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const glm::vec3 d = equirectDirection((static_cast<float>(x) + 0.5f) / static_cast<float>(out.width),
                                                  (static_cast<float>(y) + 0.5f) / static_cast<float>(out.height));
            // The face the direction leaves through: the largest component along a face's forward.
            int best = 0;
            float along = -2.0f;
            for (int f = 0; f < 6; ++f) {
                const float a = glm::dot(d, basis[static_cast<std::size_t>(f)].forward);
                if (a > along) {
                    along = a;
                    best = f;
                }
            }
            const CubeFace& face = basis[static_cast<std::size_t>(best)];
            const glm::vec3 right = glm::cross(face.forward, face.up);
            const glm::vec3 c = sampleFace(faces[static_cast<std::size_t>(best)], glm::dot(d, right) / along, glm::dot(d, face.up) / along);
            float* dst = &out.rgb[(static_cast<std::size_t>(y) * out.width + x) * 3];
            dst[0] = c.r;
            dst[1] = c.g;
            dst[2] = c.b;
        }
    }
    return out;
}

Result<int> bakeReflectionProbes(Renderer& renderer, Scene& scene, AssetLibrary& assets, RenderSettings settings,
                                 bool keepPrevious) {
    settings.reflectionProbes = false;  // what the probes see, not what they reflected last time
    settings.temporalAntiAliasing = false;
    settings.screenSpaceReflections = false;
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
    int baked = 0;
    for (auto [e, probe, world] : reg.view<ReflectionProbeComponent, WorldTransformComponent>().each()) {
        const glm::vec3 position(world.matrix[3]);
        const auto resolution = static_cast<std::uint32_t>(std::clamp(probe.resolution, 16, 1024));
        std::array<HdrImage, 6> faces;
        for (std::size_t f = 0; f < 6; ++f) {
            const CubeFace& face = cubeFaces()[f];
            const glm::vec3 right = glm::cross(face.forward, face.up);
            glm::mat4 cameraWorld(1.0f);  // the camera looks along -Z
            cameraWorld[0] = glm::vec4(right, 0.0f);
            cameraWorld[1] = glm::vec4(face.up, 0.0f);
            cameraWorld[2] = glm::vec4(-face.forward, 0.0f);
            cameraWorld[3] = glm::vec4(position, 1.0f);
            auto image = renderer.captureHdr(scene, makeCameraView(cameraWorld, 90.0f, 0.05f, 1.0f), {resolution, resolution}, settings);
            if (!image) return std::unexpected(image.error());
            faces[f] = std::move(*image);
        }
        const HdrImage equirect = cubeToEquirect(faces, resolution * 4);
        // A new name per bake: caches keyed by file see the new contents.
        const std::string key = std::format("Saved/Probes/{}-{:x}.hdr", scene.uuidOf(e).toString(),
                                            static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
        if (auto saved = saveHdrFile(assets.root() / key, equirect); !saved) return std::unexpected(saved.error());
        if (!keepPrevious && probe.baked.starts_with("Saved/Probes/") && probe.baked != key) {
            std::error_code ec;
            std::filesystem::remove(assets.root() / probe.baked, ec);
        }
        probe.baked = key;
        ++baked;
    }
    log::info("Renderer", "baked {} reflection probe(s)", baked);
    return baked;
}

} // namespace lumora::render
