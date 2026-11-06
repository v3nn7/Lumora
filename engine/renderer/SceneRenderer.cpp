#include "renderer/SceneRenderer.hpp"

#include "core/JobSystem.hpp"
#include "renderer/FrameResources.hpp"
#include "renderer/GpuTypes.hpp"
#include "renderer/RenderContext.hpp"
#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

namespace lumora::render {

Frustum Frustum::fromViewProjection(const glm::mat4& m) {
    auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    Frustum f{};
    f.planes[0] = row(3) + row(0);  // left
    f.planes[1] = row(3) - row(0);  // right
    f.planes[2] = row(3) + row(1);  // top (Vulkan clip Y points down)
    f.planes[3] = row(3) - row(1);  // bottom
    f.planes[4] = row(3) - row(2);  // near (reversed-Z: z <= w); far plane is at infinity
    for (glm::vec4& p : f.planes) {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0f) p /= len;
    }
    return f;
}

bool Frustum::intersectsAabb(const glm::vec3& center, const glm::vec3& extents) const {
    for (const glm::vec4& p : planes) {
        const glm::vec3 n(p);
        const float radius = glm::dot(glm::abs(n), extents);
        if (glm::dot(n, center) + p.w < -radius) return false;
    }
    return true;
}

ShadowFit fitSunShadow(const CameraView& camera, const glm::vec3& lightDirection, float distance,
                       std::uint32_t resolution) {
    const glm::mat4 invView = glm::inverse(camera.view);
    const glm::vec3 position(invView[3]);
    const glm::vec3 right = glm::normalize(glm::vec3(invView[0]));
    const glm::vec3 up = glm::normalize(glm::vec3(invView[1]));
    const glm::vec3 forward = -glm::normalize(glm::vec3(invView[2]));
    const float tanHalf = std::tan(camera.fovYRadians * 0.5f);

    // Bounding sphere of the frustum slice [near, distance]: rotation invariant -> stable size.
    glm::vec3 corners[8];
    int n = 0;
    for (float d : {camera.nearPlane, distance}) {
        const float h = d * tanHalf;
        const float w = h * camera.aspect;
        for (float sx : {-1.0f, 1.0f}) {
            for (float sy : {-1.0f, 1.0f}) corners[n++] = position + forward * d + right * (w * sx) + up * (h * sy);
        }
    }
    glm::vec3 center(0.0f);
    for (const glm::vec3& c : corners) center += c;
    center /= 8.0f;
    float radius = 0.0f;
    for (const glm::vec3& c : corners) radius = std::max(radius, glm::length(c - center));
    radius = std::ceil(radius * 16.0f) / 16.0f;

    const glm::vec3 dir = glm::normalize(lightDirection);
    const glm::vec3 lightUp = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    // Depth range symmetric around the slice centre (depth 0.5 = centre). Towards the sun it
    // includes occluders outside the view slice; away from it, receivers that a low sun spreads
    // far along its direction while they still project inside this cascade's square.
    constexpr float kCasterMargin = 60.0f;
    const float halfDepth = std::max(radius + kCasterMargin, 4.0f * radius);
    const glm::mat4 view = glm::lookAt(center - dir * halfDepth, center, lightUp);
    glm::mat4 projection = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * halfDepth);

    // Snap the projected world origin to whole texels.
    const glm::mat4 vp = projection * view;
    const float halfRes = static_cast<float>(resolution) * 0.5f;
    const glm::vec4 origin = vp * glm::vec4(0, 0, 0, 1) * halfRes;
    const glm::vec2 offset = (glm::round(glm::vec2(origin)) - glm::vec2(origin)) / halfRes;
    projection[3][0] += offset.x;
    projection[3][1] += offset.y;
    return {projection * view, radius};
}

std::array<float, kShadowCascades> cascadeSplits(float nearPlane, float distance, float lambda) {
    std::array<float, kShadowCascades> splits{};
    const float n = std::max(nearPlane, 0.01f);
    const float f = std::max(distance, n * 2.0f);
    const float l = std::clamp(lambda, 0.0f, 1.0f);
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(kShadowCascades);
        splits[i] = l * n * std::pow(f / n, p) + (1.0f - l) * (n + (f - n) * p);
    }
    splits.back() = f;
    return splits;
}

namespace {
constexpr VkFormat kAoFormat = VK_FORMAT_R8_UNORM;
} // namespace

SceneRenderer::SceneRenderer(RenderContext& context) : ctx_(context) {
    auto& shaders = ctx_.shaders();
    const VkFormat hdr = ctx_.hdrFormat();

    rhi::GraphicsPipelineDesc sky;
    sky.vertexShader = shaders.get("sky.vert");
    sky.fragmentShader = shaders.get("sky.frag");
    sky.colorFormats = {hdr};
    sky.layout = ctx_.pipelineLayout();
    sky.debugName = "Sky";
    sky_ = rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), sky);

    rhi::GraphicsPipelineDesc opaque;
    opaque.vertexShader = shaders.get("mesh.vert");
    opaque.fragmentShader = shaders.get("mesh.frag");
    opaque.meshVertexInput = true;
    opaque.cullMode = VK_CULL_MODE_BACK_BIT;
    opaque.depthTest = true;
    opaque.depthWrite = false;  // the depth prepass already wrote the nearest surfaces
    opaque.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    opaque.colorFormats = {hdr};
    opaque.depthFormat = ctx_.depthFormat();
    opaque.layout = ctx_.pipelineLayout();
    opaque.debugName = "OpaquePBR";
    opaque_ = rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), opaque);

    rhi::GraphicsPipelineDesc grid;
    grid.vertexShader = shaders.get("grid.vert");
    grid.fragmentShader = shaders.get("grid.frag");
    grid.depthTest = true;
    grid.depthWrite = false;
    grid.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    grid.blend = rhi::BlendMode::Alpha;
    grid.colorFormats = {hdr};
    grid.depthFormat = ctx_.depthFormat();
    grid.layout = ctx_.pipelineLayout();
    grid.debugName = "EditorGrid";
    grid_ = rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), grid);

    rhi::GraphicsPipelineDesc shadow;
    shadow.vertexShader = shaders.get("shadow.vert");
    shadow.meshVertexInput = true;
    shadow.positionOnly = true;
    shadow.cullMode = VK_CULL_MODE_NONE;  // single-sided planes still cast shadows
    shadow.depthTest = true;
    shadow.depthWrite = true;
    shadow.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;  // conventional (not reversed) depth
    shadow.depthBias = true;
    shadow.depthBiasConstant = 1.5f;
    shadow.depthBiasSlope = 2.0f;
    shadow.depthFormat = ctx_.depthFormat();
    shadow.layout = ctx_.pipelineLayout();
    shadow.debugName = "SunShadow";
    shadow_ = rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), shadow);

    rhi::GraphicsPipelineDesc prepass;
    prepass.vertexShader = shaders.get("depth.vert");
    prepass.meshVertexInput = true;
    prepass.positionOnly = true;
    prepass.cullMode = VK_CULL_MODE_BACK_BIT;
    prepass.depthTest = true;
    prepass.depthWrite = true;
    prepass.depthCompare = VK_COMPARE_OP_GREATER;
    prepass.depthFormat = ctx_.depthFormat();
    prepass.layout = ctx_.pipelineLayout();
    prepass.debugName = "DepthPrepass";
    depthPrepass_ = rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), prepass);

    for (int blur = 0; blur < 2; ++blur) {
        rhi::GraphicsPipelineDesc ao;
        ao.vertexShader = shaders.get("ssao.vert");
        ao.fragmentShader = shaders.get(blur ? "ssao.blur.frag" : "ssao.frag");
        ao.fragmentEntry = blur ? "PSBlur" : "PSOcclusion";
        ao.colorFormats = {kAoFormat};
        ao.layout = ctx_.pipelineLayout();
        ao.debugName = blur ? "SSAOBlur" : "SSAO";
        (blur ? ssaoBlur_ : ssao_) = rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), ao);
    }

    for (int up = 0; up < 2; ++up) {
        rhi::GraphicsPipelineDesc bloom;
        bloom.vertexShader = shaders.get("bloom.vert");
        bloom.fragmentShader = shaders.get(up ? "bloom.up.frag" : "bloom.down.frag");
        bloom.fragmentEntry = up ? "PSUpsample" : "PSDownsample";
        bloom.blend = up ? rhi::BlendMode::Additive : rhi::BlendMode::Opaque;
        bloom.colorFormats = {hdr};
        bloom.layout = ctx_.pipelineLayout();
        bloom.debugName = up ? "BloomUpsample" : "BloomDownsample";
        bloom_.emplace(up, rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), bloom));
    }
}

VkPipeline SceneRenderer::tonemapPipeline(VkFormat outputFormat) {
    if (auto it = tonemap_.find(outputFormat); it != tonemap_.end()) {
        return it->second.get();
    }
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = ctx_.shaders().get("tonemap.vert");
    desc.fragmentShader = ctx_.shaders().get("tonemap.frag");
    desc.colorFormats = {outputFormat};
    desc.layout = ctx_.pipelineLayout();
    desc.debugName = "Tonemap";
    return tonemap_.emplace(outputFormat, rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), desc))
        .first->second.get();
}

VkPipeline SceneRenderer::overlayPipeline(VkFormat outputFormat) {
    if (auto it = overlay_.find(outputFormat); it != overlay_.end()) {
        return it->second.get();
    }
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = ctx_.shaders().get("overlay.vert");
    desc.fragmentShader = ctx_.shaders().get("overlay.frag");
    desc.overlayVertexInput = true;
    desc.blend = rhi::BlendMode::Alpha;
    desc.colorFormats = {outputFormat};
    desc.layout = ctx_.pipelineLayout();
    desc.debugName = "Overlay";
    return overlay_.emplace(outputFormat, rhi::createGraphicsPipeline(ctx_.device(), ctx_.pipelineCache(), desc))
        .first->second.get();
}

const GpuMesh* SceneRenderer::meshFor(const MeshRendererComponent& mesh, AssetLibrary* assets, bool block) {
    if (mesh.meshAsset.empty()) return &ctx_.meshes().primitive(mesh.mesh);
    return assets != nullptr ? ctx_.meshes().asset(*assets, mesh.meshAsset, block) : nullptr;
}

void SceneRenderer::ensureObjectCapacity(FrameResources& frame, std::uint32_t count) {
    if (count <= frame.objectCapacity && frame.objects) return;
    std::uint32_t capacity = std::max(frame.objectCapacity, 256u);
    while (capacity < count) capacity *= 2;
    frame.objects = rhi::Buffer(ctx_.device(), {capacity * sizeof(GpuObject), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                rhi::MemoryUsage::CpuToGpu, "FrameObjects"});
    frame.objectCapacity = capacity;
    const VkDescriptorBufferInfo info{frame.objects.handle(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = frame.set;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(ctx_.device().handle(), 1, &write, 0, nullptr);
}

SceneDrawStats SceneRenderer::build(RenderGraph& graph, FrameResources& frame, const Scene& scene,
                                    const CameraView& camera, const RenderSettings& settings, RGImage output,
                                    VkFormat outputFormat, VkExtent2D extent, double timeSeconds) {
    SceneDrawStats stats;
    const auto& reg = scene.registry();
    const EnvironmentSettings& env = scene.environment();

    // ---- Frame constants -------------------------------------------------------------------
    FrameConstants fc;
    fc.view = camera.view;
    fc.projection = camera.projection;
    fc.viewProjection = camera.projection * camera.view;
    fc.inverseViewProjection = glm::inverse(fc.viewProjection);
    fc.cameraPositionTime = glm::vec4(camera.position, static_cast<float>(timeSeconds));
    fc.viewportSize = glm::vec4(static_cast<float>(extent.width), static_cast<float>(extent.height),
                                1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height));
    fc.skyZenith = glm::vec4(env.skyZenithColor, env.ambientIntensity);
    fc.skyHorizon = glm::vec4(env.skyHorizonColor, env.exposure);
    fc.groundColor = glm::vec4(env.groundColor, 0.0f);
    fc.gridParams = glm::vec4(settings.showGrid ? 1.0f : 0.0f, settings.gridCellSize, settings.gridFadeDistance, 0.0f);
    fc.fogParams = glm::vec4(std::max(env.fogDensity, 0.0f), std::max(env.fogHeightFalloff, 1e-4f), 0.97f, 0.0f);
    // Image-based lighting from the scene's HDR sky (procedural sky until it has loaded).
    if (!env.skyTexture.empty() && settings.assets != nullptr) {
        if (const auto* gpu = ctx_.environments().get(*settings.assets, env.skyTexture, settings.blockOnAssets)) {
            fc.environmentParams = glm::vec4(1.0f, std::max(env.skyIntensity, 0.0f), glm::radians(env.skyRotationDeg),
                                             static_cast<float>(gpu->specularLevels - 1));
            fc.environmentTextures = glm::uvec4(gpu->skyIndex, gpu->specularIndex, ctx_.materialSampler(), 0u);
            for (std::size_t i = 0; i < gpu->irradiance.size(); ++i) fc.irradianceSh[i] = glm::vec4(gpu->irradiance[i], 0.0f);
            stats.environmentLit = true;
        }
    }

    std::uint32_t lightCount = 0;
    bool haveSun = false;
    std::uint32_t sunIndex = UINT32_MAX;
    glm::vec3 sunTravel(0.0f, -1.0f, 0.0f);
    for (auto [e, light, world] : reg.view<LightComponent, WorldTransformComponent>().each()) {
        if (lightCount >= kMaxLights) break;
        GpuLight& gl = fc.lights[lightCount++];
        const glm::vec3 position(world.matrix[3]);
        const glm::vec3 direction = glm::normalize(glm::vec3(world.matrix * glm::vec4(0, 0, -1, 0)));
        gl.positionRange = glm::vec4(position, light.range);
        gl.colorIntensity = glm::vec4(light.color, light.intensity);
        gl.directionType = glm::vec4(direction, static_cast<float>(light.type));
        if (light.type == LightType::Directional && !haveSun) {
            haveSun = true;
            sunIndex = lightCount - 1;
            sunTravel = direction;
            fc.sunDirection = glm::vec4(-direction, 1.0f);
            fc.sunColor = glm::vec4(light.color * light.intensity, 1.0f);
        }
    }
    const bool shadows = settings.shadows && haveSun && settings.shadowResolution > 0 && settings.shadowDistance > 0.0f;
    fc.counts = glm::uvec4(lightCount, shadows ? sunIndex : UINT32_MAX, ctx_.materialSampler(), 0);
    stats.lights = lightCount;
    // Cascades are the 2x2 tiles of one atlas; each covers the view frustum from the near plane
    // to its split (nested), so every visible point within the shadow distance has a cascade.
    const std::uint32_t tile =
        std::min(settings.shadowResolution, ctx_.device().caps().maxImageDimension2D / 2);
    const std::uint32_t atlasSize = tile * 2;
    std::array<glm::mat4, kShadowCascades> cascadeMatrices{};
    if (shadows) {
        const auto splits = cascadeSplits(camera.nearPlane, settings.shadowDistance, settings.cascadeSplitLambda);
        for (std::uint32_t c = 0; c < kShadowCascades; ++c) {
            const ShadowFit fit = fitSunShadow(camera, sunTravel, splits[c], tile);
            cascadeMatrices[c] = fit.viewProjection;
            fc.cascadeViewProjection[c] = fit.viewProjection;
            fc.cascadeTexelWorld[static_cast<int>(c)] = 2.0f * fit.radius / static_cast<float>(tile);
        }
        fc.shadowParams = glm::vec4(1.0f, static_cast<float>(ctx_.shadowCompareSampler()),
                                    1.0f / static_cast<float>(atlasSize), static_cast<float>(atlasSize));
        fc.shadowFade = glm::vec4(settings.shadowDistance * 0.85f, settings.shadowDistance, 0.0f, 0.0f);
        stats.shadowsRendered = true;
    }
    frame.constants.write(std::as_bytes(std::span(&fc, 1)));

    // ---- Objects + culling -----------------------------------------------------------------
    struct Draw {
        const GpuMesh* mesh;
        std::uint32_t object;
        glm::vec3 center;  // world AABB
        glm::vec3 extent;
    };
    std::vector<Draw> draws;    // visible to the camera
    std::vector<Draw> casters;  // may cast into a shadow cascade (also off-screen objects)
    std::vector<GpuObject> objects;
    const Frustum frustum = Frustum::fromViewProjection(fc.viewProjection);

    // 1. Gather (serial: the ECS view and asset lookups, which may start loads, are not thread-safe).
    struct Candidate {
        entt::entity entity;
        const MeshRendererComponent* mesh;
        const glm::mat4* world;
        const GpuMesh* gpuMesh;
    };
    std::vector<Candidate> candidates;
    for (auto [e, mesh, world] : reg.view<MeshRendererComponent, WorldTransformComponent>().each()) {
        if (!mesh.visible) continue;
        const GpuMesh* gpuMeshPtr = meshFor(mesh, settings.assets, settings.blockOnAssets);
        if (gpuMeshPtr == nullptr) continue;  // still loading, or unresolved (already reported)
        candidates.push_back({e, &mesh, &world.matrix, gpuMeshPtr});
    }

    // 2. Bounds, culling and per-object constants on the job system's threads (pure math).
    struct Prepared {
        glm::vec3 center;
        glm::vec3 extent;
        bool visible;
        GpuObject object;
    };
    std::vector<Prepared> prepared(candidates.size());
    JobSystem::global().parallelFor(candidates.size(), 512, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const Candidate& c = candidates[i];
            const glm::mat4& world = *c.world;
            const glm::vec3 localCenter = (c.gpuMesh->bounds.min + c.gpuMesh->bounds.max) * 0.5f;
            const glm::vec3 localExtent = (c.gpuMesh->bounds.max - c.gpuMesh->bounds.min) * 0.5f;
            Prepared& p = prepared[i];
            p.center = glm::vec3(world * glm::vec4(localCenter, 1.0f));
            const glm::mat3 m(world);
            p.extent = glm::abs(m[0]) * localExtent.x + glm::abs(m[1]) * localExtent.y + glm::abs(m[2]) * localExtent.z;
            p.visible = !settings.frustumCulling || frustum.intersectsAabb(p.center, p.extent);
            if (!p.visible && !shadows) continue;
            const MeshRendererComponent& mesh = *c.mesh;
            GpuObject& obj = p.object;
            obj.world = world;
            obj.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world))));
            obj.baseColorMetallic = glm::vec4(mesh.baseColor, mesh.metallic);
            obj.emissiveRoughness = glm::vec4(mesh.emissive * mesh.emissiveStrength, mesh.roughness);
            obj.flags = glm::vec4(c.entity == settings.selected ? 1.0f : 0.0f, static_cast<float>(mesh.pattern), 0.0f, 0.0f);
        }
    });

    // 3. Textures (serial: the cache may start uploads) and the draw lists, in scene order.
    objects.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        Prepared& p = prepared[i];
        if (!p.visible) {
            ++stats.culledObjects;
            if (!shadows) continue;
        }
        GpuObject& obj = p.object;
        if (p.visible && settings.assets != nullptr) {  // casters only need depth, not materials
            const MeshRendererComponent& mesh = *candidates[i].mesh;
            TextureCache& textures = ctx_.textures();
            const bool block = settings.blockOnAssets;
            obj.textures = glm::uvec4(textures.index(*settings.assets, mesh.baseColorTexture, TextureColorSpace::Srgb, block),
                                      textures.index(*settings.assets, mesh.metallicRoughnessTexture, TextureColorSpace::Linear, block),
                                      textures.index(*settings.assets, mesh.emissiveTexture, TextureColorSpace::Srgb, block),
                                      textures.index(*settings.assets, mesh.normalTexture, TextureColorSpace::Linear, block));
        }
        const Draw draw{candidates[i].gpuMesh, static_cast<std::uint32_t>(objects.size()), p.center, p.extent};
        if (p.visible) draws.push_back(draw);
        if (shadows) casters.push_back(draw);
        objects.push_back(obj);
    }
    std::ranges::sort(draws, std::less<>{}, [](const Draw& d) { return d.mesh; });
    std::ranges::sort(casters, std::less<>{}, [](const Draw& d) { return d.mesh; });
    ensureObjectCapacity(frame, static_cast<std::uint32_t>(std::max<std::size_t>(objects.size(), 1)));
    if (!objects.empty()) {
        frame.objects.write(std::as_bytes(std::span(objects)));
    }
    stats.visibleObjects = static_cast<std::uint32_t>(draws.size());
    for (const Draw& d : draws) {
        stats.triangles += d.mesh->indexCount / 3;
    }
    stats.drawCalls = static_cast<std::uint32_t>(draws.size()) * 2 + 2 + (settings.showGrid ? 1u : 0u);  // prepass + shading

    // ---- Render graph ----------------------------------------------------------------------
    const RGImage hdr = graph.createImage("SceneColorHDR", {ctx_.hdrFormat(), extent});
    const RGImage depth = graph.createImage("SceneDepth", {ctx_.depthFormat(), extent});
    const VkPipelineLayout layout = ctx_.pipelineLayout();
    const VkDescriptorSet sets[2] = {frame.set, ctx_.bindless().set()};
    auto bindSets = [layout, sets](VkCommandBuffer cmd) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
    };

    RGImage shadowMap{};
    if (shadows) {
        // Per-cascade caster culling against the cascade's side planes. Casters between the sun
        // and the cascade are kept: the light-facing plane never culls. One cascade per thread.
        std::array<std::vector<Draw>, kShadowCascades> cascadeCasters;
        JobSystem::global().parallelFor(kShadowCascades, 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t c = begin; c < end; ++c) {
                Frustum box = Frustum::fromViewProjection(cascadeMatrices[c]);
                box.planes[4] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                for (const Draw& d : casters) {
                    if (box.intersectsAabb(d.center, d.extent)) cascadeCasters[c].push_back(d);
                }
            }
        });
        std::size_t casterDraws = 0;
        for (const auto& list : cascadeCasters) casterDraws += list.size();
        stats.drawCalls += static_cast<std::uint32_t>(casterDraws);
        const bool parallelShadow = casterDraws >= settings.parallelRecordingMinDraws;
        stats.parallelRecording = stats.parallelRecording || parallelShadow;
        shadowMap = graph.createImage("SunShadowAtlas", {ctx_.depthFormat(), {atlasSize, atlasSize}});
        graph.addRasterPass(
            "SunShadow",
            [&](PassBuilder& b) {
                b.writeDepth(shadowMap, LoadOp::Clear, 1.0f);
                b.recordInParallel(parallelShadow);
            },
            [this, bindSets, layout, tile, cascadeCasters = std::move(cascadeCasters)](PassContext& ctx) {
                // One cascade per secondary command buffer when recorded in parallel.
                ctx.recordParallel(kShadowCascades, 1, [&](VkCommandBuffer cmd, std::size_t begin, std::size_t end) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_.get());
                    bindSets(cmd);
                    for (std::size_t c = begin; c < end; ++c) {
                        const auto x = static_cast<std::int32_t>((c % 2) * tile);
                        const auto y = static_cast<std::int32_t>((c / 2) * tile);
                        const VkViewport viewport{static_cast<float>(x), static_cast<float>(y), static_cast<float>(tile),
                                                  static_cast<float>(tile), 0.0f, 1.0f};
                        const VkRect2D scissor{{x, y}, {tile, tile}};
                        vkCmdSetViewport(cmd, 0, 1, &viewport);
                        vkCmdSetScissor(cmd, 0, 1, &scissor);
                        const GpuMesh* mesh = nullptr;
                        for (const Draw& d : cascadeCasters[c]) {
                            if (d.mesh != mesh) {
                                mesh = d.mesh;
                                const VkBuffer vb = mesh->vertices.handle();
                                const VkDeviceSize offset = 0;
                                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                                vkCmdBindIndexBuffer(cmd, mesh->indices.handle(), 0, VK_INDEX_TYPE_UINT32);
                            }
                            PushConstants pc;
                            pc.index0 = d.object;
                            pc.index1 = static_cast<std::uint32_t>(c);
                            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                               sizeof(pc), &pc);
                            vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
                        }
                    }
                });
            });
    }

    // ---- Depth prepass, then ambient occlusion from it ---------------------------------------
    const bool parallelPrepass = draws.size() >= settings.parallelRecordingMinDraws;
    graph.addRasterPass(
        "DepthPrepass",
        [&](PassBuilder& b) {
            b.writeDepth(depth, LoadOp::Clear, 0.0f);  // reversed-Z: 0 is the far plane
            b.recordInParallel(parallelPrepass);
        },
        [this, bindSets, layout, draws](PassContext& ctx) {  // a copy: runs after build() returns
            if (draws.empty()) return;
            ctx.recordParallel(draws.size(), 64, [&](VkCommandBuffer cmd, std::size_t begin, std::size_t end) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPrepass_.get());
                bindSets(cmd);
                const GpuMesh* mesh = nullptr;
                for (std::size_t i = begin; i < end; ++i) {
                    const Draw& d = draws[i];
                    if (d.mesh != mesh) {
                        mesh = d.mesh;
                        const VkBuffer vb = mesh->vertices.handle();
                        const VkDeviceSize offset = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                        vkCmdBindIndexBuffer(cmd, mesh->indices.handle(), 0, VK_INDEX_TYPE_UINT32);
                    }
                    PushConstants pc;
                    pc.index0 = d.object;
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
                }
            });
        });

    RGImage ao{};
    const bool ambientOcclusion = settings.ambientOcclusion && !draws.empty() && extent.width >= 8 && extent.height >= 8;
    if (ambientOcclusion) {
        const VkExtent2D half{std::max(extent.width / 2, 1u), std::max(extent.height / 2, 1u)};
        const RGImage aoRaw = graph.createImage("SSAORaw", {kAoFormat, half});
        ao = graph.createImage("SSAO", {kAoFormat, extent});
        const std::uint32_t linear = ctx_.linearClampSampler();
        graph.addRasterPass(
            "SSAO",
            [&](PassBuilder& b) {
                b.writeColor(aoRaw, LoadOp::DontCare);
                b.sample(depth);
            },
            [this, bindSets, layout, depth, radius = settings.aoRadius, intensity = settings.aoIntensity](PassContext& ctx) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssao_.get());
                bindSets(ctx.cmd);
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(depth);
                pc.param0 = radius;
                pc.param1 = intensity;
                pc.param2 = 0.02f;
                vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            });
        graph.addRasterPass(
            "SSAOBlur",
            [&](PassBuilder& b) {
                b.writeColor(ao, LoadOp::DontCare);
                b.sample(aoRaw);
                b.sample(depth);
            },
            [this, bindSets, layout, depth, aoRaw, linear](PassContext& ctx) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssaoBlur_.get());
                bindSets(ctx.cmd);
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(depth);
                pc.index1 = ctx.bindlessIndex(aoRaw);
                pc.index2 = linear;
                vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            });
        stats.ambientOcclusion = true;
    }

    graph.addRasterPass(
        "Sky", [&](PassBuilder& b) { b.writeColor(hdr, LoadOp::DontCare); },
        [this, bindSets](PassContext& ctx) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_.get());
            bindSets(ctx.cmd);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });

    const bool parallelOpaque = draws.size() >= settings.parallelRecordingMinDraws;
    stats.parallelRecording = stats.parallelRecording || parallelOpaque;
    graph.addRasterPass(
        "Opaque",
        [&](PassBuilder& b) {
            b.writeColor(hdr, LoadOp::Load);
            b.readDepth(depth);  // from the prepass; the opaque pass only tests against it
            if (shadows) b.sample(shadowMap);
            if (ambientOcclusion) b.sample(ao);
            b.recordInParallel(parallelOpaque);
        },
        [this, bindSets, layout, shadows, shadowMap, ambientOcclusion, ao, debugCascades = settings.debugCascades,
         draws = std::move(draws)](PassContext& ctx) {
            if (draws.empty()) return;
            const std::uint32_t shadowIndex = shadows ? ctx.bindlessIndex(shadowMap) : 0u;
            const std::uint32_t aoIndex = ambientOcclusion ? ctx.bindlessIndex(ao) : 0u;
            const std::uint32_t flags =
                (shadows ? 1u : 0u) | (shadows && debugCascades ? 2u : 0u) | (ambientOcclusion ? 4u : 0u);
            // Draws are sorted by mesh, so each chunk rebinds buffers only where meshes change.
            ctx.recordParallel(draws.size(), 64, [&](VkCommandBuffer cmd, std::size_t begin, std::size_t end) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_.get());
                bindSets(cmd);
                const GpuMesh* mesh = nullptr;
                for (std::size_t i = begin; i < end; ++i) {
                    const Draw& d = draws[i];
                    if (d.mesh != mesh) {
                        mesh = d.mesh;
                        const VkBuffer vb = mesh->vertices.handle();
                        const VkDeviceSize offset = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                        vkCmdBindIndexBuffer(cmd, mesh->indices.handle(), 0, VK_INDEX_TYPE_UINT32);
                    }
                    PushConstants pc;
                    pc.index0 = d.object;
                    pc.index1 = shadowIndex;
                    pc.index2 = aoIndex;
                    pc.flags = flags;
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
                }
            });
        });

    if (settings.showGrid) {
        graph.addRasterPass(
            "Grid",
            [&](PassBuilder& b) {
                b.writeColor(hdr, LoadOp::Load);
                b.readDepth(depth);
            },
            [this, bindSets](PassContext& ctx) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_.get());
                bindSets(ctx.cmd);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            });
    }

    // ---- Bloom: downsample chain, then additive tent upsample back to the half-res level.
    const std::uint32_t sampler = ctx_.linearClampSampler();
    std::vector<RGImage> bloomChain;
    if (settings.bloom && env.bloomIntensity > 0.0f) {
        VkExtent2D size = extent;
        while (bloomChain.size() < 6 && size.width >= 16 && size.height >= 16) {
            size = {size.width / 2, size.height / 2};
            bloomChain.push_back(graph.createImage("Bloom" + std::to_string(bloomChain.size()), {ctx_.hdrFormat(), size}));
        }
    }
    const VkPipeline bloomDown = bloom_.at(0).get();
    const VkPipeline bloomUp = bloom_.at(1).get();
    for (std::size_t i = 0; i < bloomChain.size(); ++i) {
        const RGImage source = i == 0 ? hdr : bloomChain[i - 1];
        const RGImage target = bloomChain[i];
        graph.addRasterPass(
            "BloomDown" + std::to_string(i),
            [&](PassBuilder& b) {
                b.sample(source);
                b.writeColor(target, LoadOp::DontCare);
            },
            [=](PassContext& ctx) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomDown);
                bindSets(ctx.cmd);
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(source);
                pc.index1 = sampler;
                pc.flags = i == 0 ? 1u : 0u;
                vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            });
    }
    for (std::size_t i = bloomChain.size(); i-- > 1;) {
        const RGImage source = bloomChain[i];
        const RGImage target = bloomChain[i - 1];
        const VkExtent2D sourceSize = {std::max(extent.width >> (i + 1), 1u), std::max(extent.height >> (i + 1), 1u)};
        const float radius = 1.0f / static_cast<float>(std::max(sourceSize.width, 1u));
        graph.addRasterPass(
            "BloomUp" + std::to_string(i),
            [&](PassBuilder& b) {
                b.sample(source);
                b.writeColor(target, LoadOp::Load);
            },
            [=](PassContext& ctx) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomUp);
                bindSets(ctx.cmd);
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(source);
                pc.index1 = sampler;
                pc.param0 = radius;
                vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            });
    }
    stats.drawCalls += static_cast<std::uint32_t>(bloomChain.empty() ? 0 : bloomChain.size() * 2 - 1);

    const VkPipeline tonemap = tonemapPipeline(outputFormat);
    const bool shaderSrgb = !rhi::isSrgbFormat(outputFormat);
    const bool haveBloom = !bloomChain.empty();
    const RGImage bloomResult = haveBloom ? bloomChain.front() : RGImage{};
    // Bloom is a lerp towards the blurred image; normalise for the summed chain levels.
    const float bloomStrength = haveBloom ? std::clamp(env.bloomIntensity, 0.0f, 1.0f) : 0.0f;
    const float bloomNormalisation = haveBloom ? 1.0f / static_cast<float>(bloomChain.size()) : 1.0f;
    const float exposure = env.exposure;
    const float vignette = std::clamp(env.vignette, 0.0f, 1.0f);
    graph.addRasterPass(
        "Tonemap",
        [&](PassBuilder& b) {
            b.sample(hdr);
            if (haveBloom) b.sample(bloomResult);
            b.writeColor(output, LoadOp::DontCare);
        },
        [=](PassContext& ctx) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap);
            bindSets(ctx.cmd);
            PushConstants pc;
            pc.index0 = ctx.bindlessIndex(hdr);
            pc.index1 = sampler;
            pc.index2 = haveBloom ? ctx.bindlessIndex(bloomResult) : 0u;
            pc.flags = (shaderSrgb ? 1u : 0u) | (haveBloom ? 2u : 0u);
            pc.param0 = exposure;
            pc.param1 = bloomStrength;
            pc.param2 = vignette;
            pc.param3 = bloomNormalisation;
            vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });

    // ---- Overlay (gizmos): unlit, alpha blended, on top of the final image.
    if (settings.overlay != nullptr && !settings.overlay->empty()) {
        const auto& vertices = settings.overlay->vertices;
        const VkDeviceSize bytes = vertices.size() * sizeof(OverlayVertex);
        if (frame.overlayVertices.size() < bytes) {
            VkDeviceSize capacity = std::max<VkDeviceSize>(64 * 1024, frame.overlayVertices.size());
            while (capacity < bytes) capacity *= 2;
            frame.overlayVertices = rhi::Buffer(ctx_.device(), {capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                                rhi::MemoryUsage::CpuToGpu, "OverlayVertices"});
        }
        frame.overlayVertices.write(std::as_bytes(std::span(vertices)));
        const VkPipeline overlay = overlayPipeline(outputFormat);
        const VkBuffer vb = frame.overlayVertices.handle();
        const auto count = static_cast<std::uint32_t>(vertices.size());
        graph.addRasterPass(
            "Overlay", [&](PassBuilder& b) { b.writeColor(output, LoadOp::Load); },
            [=](PassContext& ctx) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay);
                bindSets(ctx.cmd);
                PushConstants pc;
                pc.flags = shaderSrgb ? 1u : 0u;
                vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
                vkCmdDraw(ctx.cmd, count, 1, 0, 0);
            });
        ++stats.drawCalls;
    }
    return stats;
}

} // namespace lumora::render
