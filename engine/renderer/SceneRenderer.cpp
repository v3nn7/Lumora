#include "renderer/SceneRenderer.hpp"

#include "renderer/FrameResources.hpp"
#include "renderer/GpuTypes.hpp"
#include "renderer/RenderContext.hpp"
#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <span>

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
    constexpr float kCasterMargin = 60.0f;  // include occluders outside the view slice
    const glm::mat4 view = glm::lookAt(center - dir * (radius + kCasterMargin), center, lightUp);
    glm::mat4 projection = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + kCasterMargin);

    // Snap the projected world origin to whole texels.
    const glm::mat4 vp = projection * view;
    const float halfRes = static_cast<float>(resolution) * 0.5f;
    const glm::vec4 origin = vp * glm::vec4(0, 0, 0, 1) * halfRes;
    const glm::vec2 offset = (glm::round(glm::vec2(origin)) - glm::vec2(origin)) / halfRes;
    projection[3][0] += offset.x;
    projection[3][1] += offset.y;
    return {projection * view, radius};
}

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
    opaque.depthWrite = true;
    opaque.depthCompare = VK_COMPARE_OP_GREATER;
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
    const bool shadows = settings.shadows && haveSun && settings.shadowResolution > 0;
    fc.counts = glm::uvec4(lightCount, shadows ? sunIndex : UINT32_MAX, 0, 0);
    stats.lights = lightCount;
    if (shadows) {
        const ShadowFit fit = fitSunShadow(camera, sunTravel, settings.shadowDistance, settings.shadowResolution);
        const float texelWorld = 2.0f * fit.radius / static_cast<float>(settings.shadowResolution);
        fc.lightViewProjection = fit.viewProjection;
        fc.shadowParams = glm::vec4(1.0f, static_cast<float>(ctx_.shadowCompareSampler()),
                                    1.0f / static_cast<float>(settings.shadowResolution), texelWorld * 1.5f);
        stats.shadowsRendered = true;
    }
    frame.constants.write(std::as_bytes(std::span(&fc, 1)));

    // ---- Objects + culling -----------------------------------------------------------------
    struct Draw {
        MeshPrimitive mesh;
        std::uint32_t object;
    };
    std::vector<Draw> draws;
    std::vector<GpuObject> objects;
    const Frustum frustum = Frustum::fromViewProjection(fc.viewProjection);
    for (auto [e, mesh, world] : reg.view<MeshRendererComponent, WorldTransformComponent>().each()) {
        if (!mesh.visible) continue;
        const GpuMesh& gpuMesh = ctx_.meshes().primitive(mesh.mesh);
        if (settings.frustumCulling) {
            const glm::vec3 localCenter = (gpuMesh.bounds.min + gpuMesh.bounds.max) * 0.5f;
            const glm::vec3 localExtent = (gpuMesh.bounds.max - gpuMesh.bounds.min) * 0.5f;
            const glm::vec3 center(world.matrix * glm::vec4(localCenter, 1.0f));
            const glm::mat3 m(world.matrix);
            const glm::vec3 extentWorld = glm::abs(m[0]) * localExtent.x + glm::abs(m[1]) * localExtent.y +
                                          glm::abs(m[2]) * localExtent.z;
            if (!frustum.intersectsAabb(center, extentWorld)) {
                ++stats.culledObjects;
                continue;
            }
        }
        GpuObject obj;
        obj.world = world.matrix;
        obj.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world.matrix))));
        obj.baseColorMetallic = glm::vec4(mesh.baseColor, mesh.metallic);
        obj.emissiveRoughness = glm::vec4(mesh.emissive * mesh.emissiveStrength, mesh.roughness);
        obj.flags = glm::vec4(e == settings.selected ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        draws.push_back({mesh.mesh, static_cast<std::uint32_t>(objects.size())});
        objects.push_back(obj);
    }
    std::ranges::sort(draws, {}, [](const Draw& d) { return static_cast<int>(d.mesh); });
    ensureObjectCapacity(frame, static_cast<std::uint32_t>(std::max<std::size_t>(objects.size(), 1)));
    if (!objects.empty()) {
        frame.objects.write(std::as_bytes(std::span(objects)));
    }
    stats.visibleObjects = static_cast<std::uint32_t>(objects.size());
    for (const Draw& d : draws) {
        stats.triangles += ctx_.meshes().primitive(d.mesh).indexCount / 3;
    }
    stats.drawCalls = static_cast<std::uint32_t>(draws.size()) + 2 + (settings.showGrid ? 1u : 0u);

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
        shadowMap = graph.createImage("SunShadowMap",
                                      {ctx_.depthFormat(), {settings.shadowResolution, settings.shadowResolution}});
        graph.addRasterPass(
            "SunShadow", [&](PassBuilder& b) { b.writeDepth(shadowMap, LoadOp::Clear, 1.0f); },
            [this, bindSets, layout, casters = draws](PassContext& ctx) {
                if (casters.empty()) return;
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_.get());
                bindSets(ctx.cmd);
                int boundMesh = -1;
                const GpuMesh* mesh = nullptr;
                for (const Draw& d : casters) {
                    if (static_cast<int>(d.mesh) != boundMesh) {
                        boundMesh = static_cast<int>(d.mesh);
                        mesh = &ctx_.meshes().primitive(d.mesh);
                        const VkBuffer vb = mesh->vertices.handle();
                        const VkDeviceSize offset = 0;
                        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
                        vkCmdBindIndexBuffer(ctx.cmd, mesh->indices.handle(), 0, VK_INDEX_TYPE_UINT32);
                    }
                    PushConstants pc;
                    pc.index0 = d.object;
                    vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(pc), &pc);
                    vkCmdDrawIndexed(ctx.cmd, mesh->indexCount, 1, 0, 0, 0);
                }
            });
        stats.drawCalls += static_cast<std::uint32_t>(draws.size());
    }

    graph.addRasterPass(
        "Sky", [&](PassBuilder& b) { b.writeColor(hdr, LoadOp::DontCare); },
        [this, bindSets](PassContext& ctx) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_.get());
            bindSets(ctx.cmd);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });

    graph.addRasterPass(
        "Opaque",
        [&](PassBuilder& b) {
            b.writeColor(hdr, LoadOp::Load);
            b.writeDepth(depth, LoadOp::Clear, 0.0f);  // reversed-Z: 0 is the far plane
            if (shadows) b.sample(shadowMap);
        },
        [this, bindSets, layout, shadows, shadowMap, draws = std::move(draws)](PassContext& ctx) {
            if (draws.empty()) return;
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_.get());
            bindSets(ctx.cmd);
            int boundMesh = -1;
            const GpuMesh* mesh = nullptr;
            for (const Draw& d : draws) {
                if (static_cast<int>(d.mesh) != boundMesh) {
                    boundMesh = static_cast<int>(d.mesh);
                    mesh = &ctx_.meshes().primitive(d.mesh);
                    const VkBuffer vb = mesh->vertices.handle();
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
                    vkCmdBindIndexBuffer(ctx.cmd, mesh->indices.handle(), 0, VK_INDEX_TYPE_UINT32);
                }
                PushConstants pc;
                pc.index0 = d.object;
                pc.index1 = shadows ? ctx.bindlessIndex(shadowMap) : 0u;
                pc.flags = shadows ? 1u : 0u;
                vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                vkCmdDrawIndexed(ctx.cmd, mesh->indexCount, 1, 0, 0, 0);
            }
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

    const VkPipeline tonemap = tonemapPipeline(outputFormat);
    const bool shaderSrgb = !rhi::isSrgbFormat(outputFormat);
    const float exposure = env.exposure;
    const std::uint32_t sampler = ctx_.linearClampSampler();
    graph.addRasterPass(
        "Tonemap",
        [&](PassBuilder& b) {
            b.sample(hdr);
            b.writeColor(output, LoadOp::DontCare);
        },
        [=](PassContext& ctx) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap);
            bindSets(ctx.cmd);
            PushConstants pc;
            pc.index0 = ctx.bindlessIndex(hdr);
            pc.index1 = sampler;
            pc.param0 = exposure;
            pc.flags = shaderSrgb ? 1u : 0u;
            vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });
    return stats;
}

} // namespace lumora::render
