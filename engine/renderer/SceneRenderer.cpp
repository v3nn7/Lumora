#include "renderer/SceneRenderer.hpp"

#include "assets/AssetLibrary.hpp"

#include "core/JobSystem.hpp"
#include "renderer/FrameResources.hpp"
#include "renderer/GpuTypes.hpp"
#include "renderer/LightGrid.hpp"
#include "renderer/RenderContext.hpp"
#include "scene/Landscape.hpp"
#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <bit>
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
constexpr nvrhi::Format kAoFormat = nvrhi::Format::R8_UNORM;

/// Joint matrices of a skinned mesh entity: inverse(mesh world) * joint world * inverse bind, for
/// every joint of its skin, appended to `out`. The joints are the entities that mirror the skin's
/// joint nodes under the same model root. False (palette of identities) if anything is missing.
bool appendSkinPalette(const Scene& scene, entt::entity entity, const MeshRendererComponent& mesh, AssetLibrary* assets,
                       std::vector<glm::mat4>& out) {
    const auto& reg = scene.registry();
    const auto* skinComponent = reg.try_get<SkinComponent>(entity);
    const ImportedModel* model = nullptr;
    if (skinComponent != nullptr && assets != nullptr) {
        if (auto ref = assets->resolveMeshKey(mesh.meshAsset)) {
            if (auto loaded = assets->model(ref->file)) model = *loaded;
        }
    }
    if (model == nullptr || skinComponent->skin < 0 || static_cast<std::size_t>(skinComponent->skin) >= model->skins.size()) {
        out.push_back(glm::mat4(1.0f));
        return false;
    }
    const ImportedSkin& skin = model->skins[static_cast<std::size_t>(skinComponent->skin)];
    // The model root: the first ancestor that does not mirror a model node (the mesh entity is a
    // node itself, or a material part under one).
    entt::entity root = scene.parentOf(entity);
    while (root != entt::null && reg.all_of<ModelNodeComponent>(root)) root = scene.parentOf(root);
    std::vector<entt::entity> nodes(model->nodes.size(), entt::entity{entt::null});
    auto collect = [&](auto& self, entt::entity e) -> void {
        for (entt::entity child : scene.childrenOf(e)) {
            if (const auto* node = reg.try_get<ModelNodeComponent>(child)) {
                if (node->node >= 0 && static_cast<std::size_t>(node->node) < nodes.size()) {
                    nodes[static_cast<std::size_t>(node->node)] = child;
                }
            }
            self(self, child);
        }
    };
    if (root != entt::null) collect(collect, root);
    const glm::mat4 toMesh = glm::inverse(scene.worldMatrix(entity));
    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
        const int node = skin.joints[j];
        const entt::entity joint = node >= 0 && static_cast<std::size_t>(node) < nodes.size() ? nodes[static_cast<std::size_t>(node)] : entt::null;
        out.push_back(joint != entt::null ? toMesh * scene.worldMatrix(joint) * skin.inverseBind[j] : glm::mat4(1.0f));
    }
    return true;
}

/// A mesh's vertex (and, when skinned, skin) stream and its index buffer.
void bindMesh(nvrhi::GraphicsState& state, const GpuMesh& mesh) {
    state.vertexBuffers.resize(0);
    state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(mesh.vertices).setSlot(0));
    if (mesh.skinned()) state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(mesh.skin).setSlot(1));
    state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(mesh.indices).setFormat(nvrhi::Format::R32_UINT);
}

/// Draws `vertexCount` vertices (or indices) with these push constants.
void push(nvrhi::ICommandList* cmd, const PushConstants& pc) { cmd->setPushConstants(&pc, sizeof(pc)); }

/// A full-screen triangle with one pipeline and its push constants.
void fullscreen(nvrhi::ICommandList* cmd, nvrhi::GraphicsState state, const PushConstants& pc) {
    cmd->setGraphicsState(state);
    push(cmd, pc);
    cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
}
} // namespace

SceneRenderer::SceneRenderer(RenderContext& context) : ctx_(context) {
    const nvrhi::Format hdr = ctx_.hdrFormat();
    const nvrhi::Format depth = ctx_.depthFormat();
    auto make = [&](PipelineDesc d) { return ctx_.createPipeline(d); };

    PipelineDesc sky;
    sky.name = "Sky";
    sky.vertexShader = "sky.vert";
    sky.pixelShader = "sky.frag";
    sky.colorFormats = {hdr};
    sky_ = make(sky);

    PipelineDesc opaque;
    opaque.name = "OpaquePBR";
    opaque.vertexShader = "mesh.vert";
    opaque.pixelShader = "mesh.frag";
    opaque.input = VertexInput::Mesh;
    opaque.cull = nvrhi::RasterCullMode::Back;
    opaque.depthTest = true;
    opaque.depthWrite = false;  // the depth prepass already wrote the nearest surfaces
    opaque.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
    opaque.colorFormats = {hdr};
    opaque.depthFormat = depth;
    opaque_ = make(opaque);
    opaque.name = "OpaquePBRSkinned";
    opaque.vertexShader = "mesh.skinned.vert";
    opaque.input = VertexInput::MeshSkinned;
    opaqueSkinned_ = make(opaque);

    PipelineDesc grid;
    grid.name = "EditorGrid";
    grid.vertexShader = "grid.vert";
    grid.pixelShader = "grid.frag";
    grid.depthTest = true;
    grid.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
    grid.blend = BlendMode::Alpha;
    grid.colorFormats = {hdr};
    grid.depthFormat = depth;
    grid_ = make(grid);

    PipelineDesc particles;
    particles.name = "ParticlesAlpha";
    particles.vertexShader = "particle.vert";
    particles.pixelShader = "particle.frag";
    particles.input = VertexInput::Particle;
    particles.depthTest = true;
    particles.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
    particles.blend = BlendMode::Alpha;
    particles.colorFormats = {hdr};
    particles.depthFormat = depth;
    particlesAlpha_ = make(particles);
    particles.name = "ParticlesAdditive";
    particles.blend = BlendMode::Additive;
    particlesAdditive_ = make(particles);

    PipelineDesc shadow;
    shadow.name = "SunShadow";
    shadow.vertexShader = "shadow.vert";
    shadow.input = VertexInput::Mesh;
    shadow.cull = nvrhi::RasterCullMode::None;  // single-sided planes still cast shadows
    shadow.depthTest = true;
    shadow.depthWrite = true;
    shadow.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;  // conventional (not reversed) depth
    shadow.depthBias = 2;
    shadow.slopeScaledDepthBias = 2.0f;
    shadow.depthFormat = depth;
    shadow_ = make(shadow);
    shadow.name = "SunShadowSkinned";
    shadow.vertexShader = "shadow.skinned.vert";
    shadow.input = VertexInput::MeshSkinnedPosition;
    shadowSkinned_ = make(shadow);

    PipelineDesc prepass;
    prepass.name = "DepthPrepass";
    prepass.vertexShader = "depth.vert";
    prepass.input = VertexInput::MeshPosition;
    prepass.cull = nvrhi::RasterCullMode::Back;
    prepass.depthTest = true;
    prepass.depthWrite = true;
    prepass.depthFunc = nvrhi::ComparisonFunc::Greater;
    prepass.depthFormat = depth;
    depthPrepass_ = make(prepass);
    prepass.name = "DepthPrepassSkinned";
    prepass.vertexShader = "depth.skinned.vert";
    prepass.input = VertexInput::MeshSkinnedPosition;
    depthPrepassSkinned_ = make(prepass);

    for (int blur = 0; blur < 2; ++blur) {
        PipelineDesc ao;
        ao.name = blur ? "SSAOBlur" : "SSAO";
        ao.vertexShader = "ssao.vert";
        ao.pixelShader = blur ? "ssao.blur.frag" : "ssao.frag";
        ao.pixelEntry = blur ? "PSBlur" : "PSOcclusion";
        ao.colorFormats = {kAoFormat};
        (blur ? ssaoBlur_ : ssao_) = make(ao);
    }
    for (int up = 0; up < 2; ++up) {
        PipelineDesc bloom;
        bloom.name = up ? "BloomUpsample" : "BloomDownsample";
        bloom.vertexShader = "bloom.vert";
        bloom.pixelShader = up ? "bloom.up.frag" : "bloom.down.frag";
        bloom.pixelEntry = up ? "PSUpsample" : "PSDownsample";
        bloom.blend = up ? BlendMode::Additive : BlendMode::Opaque;
        bloom.colorFormats = {hdr};
        (up ? bloomUp_ : bloomDown_) = make(bloom);
    }
    PipelineDesc decal;
    decal.name = "Decals";
    decal.vertexShader = "decal.vert";
    decal.pixelShader = "decal.frag";
    decal.input = VertexInput::MeshPosition;
    decal.cull = nvrhi::RasterCullMode::Front;  // the box's far side: also drawn with the camera inside it
    decal.blend = BlendMode::Multiply;
    decal.colorFormats = {hdr};
    decal_ = make(decal);
    PipelineDesc water;
    water.name = "Water";
    water.vertexShader = "water.vert";
    water.pixelShader = "water.frag";
    water.input = VertexInput::MeshPosition;
    water.blend = BlendMode::Alpha;
    water.colorFormats = {hdr};
    water_ = make(water);

    PipelineDesc post;
    post.vertexShader = "postfx.vert";
    post.colorFormats = {hdr};
    post.name = "DepthOfField";
    post.pixelShader = "postfx.dof.frag";
    post.pixelEntry = "PSDepthOfField";
    depthOfField_ = make(post);
    post.name = "SunShafts";
    post.pixelShader = "postfx.shafts.frag";
    post.pixelEntry = "PSShafts";
    shafts_ = make(post);
    post.name = "SunShaftsAdd";
    post.pixelShader = "postfx.shaftsadd.frag";
    post.pixelEntry = "PSShaftsAdd";
    post.blend = BlendMode::Additive;
    shaftsAdd_ = make(post);
}

SceneRenderer::~SceneRenderer() {
    if (hudAtlasIndex_ != UINT32_MAX) ctx_.bindless().releaseTexture(hudAtlasIndex_);
}

nvrhi::IGraphicsPipeline* SceneRenderer::outputPipeline(std::unordered_map<int, nvrhi::GraphicsPipelineHandle>& cache,
                                                        const PipelineDesc& desc, nvrhi::Format format) {
    nvrhi::GraphicsPipelineHandle& pipeline = cache[static_cast<int>(format)];
    if (!pipeline) {
        PipelineDesc d = desc;
        d.colorFormats = {format};
        pipeline = ctx_.createPipeline(d);
    }
    return pipeline;
}

const GpuMesh* SceneRenderer::meshFor(const MeshRendererComponent& mesh, AssetLibrary* assets, bool block) {
    if (mesh.meshAsset.empty()) return &ctx_.meshes().primitive(mesh.mesh);
    return assets != nullptr ? ctx_.meshes().asset(*assets, mesh.meshAsset, block) : nullptr;
}

SceneDrawStats SceneRenderer::build(RenderGraph& graph, FrameResources& frame, const Scene& scene,
                                    const CameraView& camera, const RenderSettings& settings, RGImage output,
                                    nvrhi::Format outputFormat, VkExtent2D extent, double timeSeconds) {
    nvrhi::IDevice* gpu = ctx_.gpu();
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
    fc.grading = glm::vec4(std::max(env.contrast, 0.0f), std::max(env.saturation, 0.0f), std::clamp(env.temperature, -1.0f, 1.0f),
                           std::clamp(env.tint, -1.0f, 1.0f));
    fc.depthOfField = glm::vec4(std::max(env.focusDistance, 0.01f), std::max(env.focusRange, 0.01f),
                                std::clamp(env.dofBlur, 0.0f, 32.0f), camera.nearPlane);
    // Image-based lighting from the scene's HDR sky (procedural sky until it has loaded).
    if (!env.skyTexture.empty() && settings.assets != nullptr) {
        if (const auto* sky = ctx_.environments().get(*settings.assets, env.skyTexture, settings.blockOnAssets)) {
            fc.environmentParams = glm::vec4(1.0f, std::max(env.skyIntensity, 0.0f), glm::radians(env.skyRotationDeg),
                                             static_cast<float>(sky->specularLevels - 1));
            fc.environmentTextures = glm::uvec4(sky->skyIndex, sky->specularIndex, ctx_.materialSampler(), 0u);
            for (std::size_t i = 0; i < sky->irradiance.size(); ++i) fc.irradianceSh[i] = glm::vec4(sky->irradiance[i], 0.0f);
            stats.environmentLit = true;
        }
    }

    // Directional lights go into the constants (few, they light everything); point lights into
    // a storage buffer, binned into screen tiles so each pixel only visits the ones near it.
    std::uint32_t lightCount = 0;
    std::vector<GpuLight> pointLights;
    struct ShadowCandidate {
        std::uint32_t light;  // index into pointLights
        LightType type;
        float outerAngle;     // spot: half-angle (radians)
        float distance;       // to the camera
    };
    std::vector<ShadowCandidate> shadowCandidates;
    bool haveSun = false;
    std::uint32_t sunIndex = UINT32_MAX;
    glm::vec3 sunTravel(0.0f, -1.0f, 0.0f);
    for (auto [e, light, world] : reg.view<LightComponent, WorldTransformComponent>().each()) {
        GpuLight gl;
        const glm::vec3 position(world.matrix[3]);
        const glm::vec3 direction = glm::normalize(glm::vec3(world.matrix * glm::vec4(0, 0, -1, 0)));
        gl.positionRange = glm::vec4(position, light.range);
        gl.colorIntensity = glm::vec4(light.color, light.intensity);
        gl.directionType = glm::vec4(direction, static_cast<float>(light.type));
        if (light.type == LightType::Spot) {
            const float outer = glm::radians(std::clamp(light.spotAngleDeg, 1.0f, 89.0f));
            const float inner = outer * (1.0f - std::clamp(light.spotSoftness, 0.0f, 1.0f));
            gl.spot = glm::vec4(std::cos(outer), std::cos(inner) + 1e-4f, -1.0f, 0.0f);
        }
        if (light.type != LightType::Directional) {
            if (pointLights.size() < kMaxPointLights && light.intensity > 0.0f) {
                if (light.castShadows && settings.shadows) {
                    shadowCandidates.push_back({static_cast<std::uint32_t>(pointLights.size()), light.type,
                                                glm::radians(std::clamp(light.spotAngleDeg, 1.0f, 89.0f)),
                                                glm::length(position - camera.position)});
                }
                pointLights.push_back(gl);
            }
            continue;
        }
        if (lightCount >= kMaxLights) continue;
        fc.lights[lightCount++] = gl;
        if (!haveSun) {
            haveSun = true;
            sunIndex = lightCount - 1;
            sunTravel = direction;
            fc.sunDirection = glm::vec4(-direction, 1.0f);
            fc.sunColor = glm::vec4(light.color * light.intensity, 1.0f);
        }
    }
    const bool shadows = settings.shadows && haveSun && settings.shadowResolution > 0 && settings.shadowDistance > 0.0f;
    // Sun shafts: where the sun is on screen, fading out as it leaves the view.
    if (haveSun && env.sunShafts > 0.0f) {
        const glm::vec4 clip = fc.viewProjection * glm::vec4(camera.position - sunTravel * 1000.0f, 1.0f);
        if (clip.w > 0.0f) {
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            const float edge = std::max(std::abs(ndc.x), std::abs(ndc.y));
            const float visibility = std::clamp(1.6f - edge, 0.0f, 1.0f);
            fc.sunShafts = glm::vec4(ndc * 0.5f + 0.5f, env.sunShafts, visibility);
        }
    }
    // Spot and point light shadows: tiles of the local atlas to the lights nearest the camera.
    struct LocalTile {
        glm::mat4 viewProjection;
        glm::vec3 lightPosition;
        float range;
    };
    std::vector<LocalTile> localTiles;
    std::ranges::sort(shadowCandidates, std::less<>{}, &ShadowCandidate::distance);
    for (const ShadowCandidate& c : shadowCandidates) {
        const std::uint32_t needed = c.type == LightType::Spot ? 1u : 6u;
        if (localTiles.size() + needed > kLocalShadowTiles) continue;
        GpuLight& gl = pointLights[c.light];
        const glm::vec3 position(gl.positionRange);
        const float range = std::max(gl.positionRange.w, 0.1f);
        const float nearPlane = std::clamp(range * 0.005f, 0.02f, 0.5f);
        gl.spot.z = static_cast<float>(localTiles.size());
        auto add = [&](const glm::vec3& forward, const glm::vec3& up, float fov) {
            const glm::mat4 view = glm::lookAt(position, position + forward, up);
            const glm::mat4 projection = glm::perspective(fov, 1.0f, nearPlane, range);
            fc.localShadowMatrices[localTiles.size()] = projection * view;
            localTiles.push_back({projection * view, position, range});
        };
        if (c.type == LightType::Spot) {
            const glm::vec3 dir = glm::normalize(glm::vec3(gl.directionType));
            add(dir, std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0),
                std::min(2.0f * c.outerAngle + glm::radians(4.0f), glm::radians(170.0f)));
        } else {
            const float quarter = glm::half_pi<float>();
            add({1, 0, 0}, {0, 1, 0}, quarter);
            add({-1, 0, 0}, {0, 1, 0}, quarter);
            add({0, 1, 0}, {0, 0, 1}, quarter);
            add({0, -1, 0}, {0, 0, 1}, quarter);
            add({0, 0, 1}, {0, 1, 0}, quarter);
            add({0, 0, -1}, {0, 1, 0}, quarter);
        }
    }
    const bool localShadows = !localTiles.empty();
    const std::uint32_t localAtlasSize = std::min(4096u, ctx_.caps().maxImageDimension2D);
    if (localShadows) fc.localShadowParams = glm::vec4(1.0f, 1.0f / static_cast<float>(localAtlasSize), 0.0f, 0.0f);
    fc.shadowParams.y = static_cast<float>(ctx_.shadowCompareSampler());  // local shadows use it without a sun
    const bool anyShadows = shadows || localShadows;
    fc.counts = glm::uvec4(lightCount, shadows ? sunIndex : UINT32_MAX, ctx_.materialSampler(), 0);
    stats.lights = lightCount + static_cast<std::uint32_t>(pointLights.size());
    {
        const LightGrid grid = buildLightGrid(pointLights, camera.view, camera.projection, extent.width, extent.height);
        fc.lightGrid = glm::uvec4(grid.tilesX, grid.tilesY, grid.tileSize, static_cast<std::uint32_t>(pointLights.size()));
        if (pointLights.empty()) pointLights.push_back(GpuLight{});  // storage buffers may not be empty
        frame.pointLights.write(gpu, std::as_bytes(std::span(pointLights)), DynamicBuffer::Kind::Structured,
                                sizeof(GpuLight), "FramePointLights");
        std::vector<std::uint32_t> gridData = grid.data;
        if (gridData.empty()) gridData.push_back(0);
        frame.lightGrid.write(gpu, std::as_bytes(std::span(gridData)), DynamicBuffer::Kind::Structured,
                              sizeof(std::uint32_t), "FrameLightGrid");
    }
    // Cascades are the 2x2 tiles of one atlas; each covers the view frustum from the near plane
    // to its split (nested), so every visible point within the shadow distance has a cascade.
    const std::uint32_t tile =
        std::min(settings.shadowResolution, ctx_.caps().maxImageDimension2D / 2);
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
    frame.constants.write(gpu, std::as_bytes(std::span(&fc, 1)), DynamicBuffer::Kind::Constant, sizeof(FrameConstants),
                          "FrameConstants");

    // ---- Objects + culling -----------------------------------------------------------------
    struct Draw {
        const GpuMesh* mesh;
        std::uint32_t object;
        glm::vec3 center;  // world AABB
        glm::vec3 extent;
        std::uint32_t count = 1;  // instances: objects object .. object + count - 1
    };
    // Copies of one mesh in consecutive objects become one instanced draw.
    auto instanced = [](const std::vector<Draw>& list) {
        std::vector<Draw> out;
        out.reserve(list.size());
        for (const Draw& d : list) {
            if (!out.empty() && out.back().mesh == d.mesh && out.back().object + out.back().count == d.object) {
                ++out.back().count;
            } else {
                out.push_back(d);
            }
        }
        return out;
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
        std::shared_ptr<const MaterialAsset> material;  // the .lmat it uses, if any
        float pattern = -1.0f;  // overrides the surface pattern (3 = terrain)
        float extra = 0.0f;     // flags.z for the pattern (terrain: packed rock colour)
    };
    std::vector<Candidate> candidates;
    ++frameCounter_;
    const std::filesystem::path projectRoot = settings.assets != nullptr ? settings.assets->root() : std::filesystem::path();
    auto landscapeEntry = [&](entt::entity e, std::uint64_t kind) -> LandscapeEntry& {
        const std::uint64_t slot = (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&scene)) * 0x9E3779B97F4A7C15ull) ^
                                   (static_cast<std::uint64_t>(entt::to_integral(e)) << 2) ^ kind;
        LandscapeEntry& entry = landscape_[slot];
        entry.lastFrame = frameCounter_;
        return entry;
    };
    // Terrains: a procedural mesh each, grass/rock coloured by slope in the shader.
    for (auto [e, terrain, world] : reg.view<TerrainComponent, WorldTransformComponent>().each()) {
        const std::uint64_t key = terrainKey(terrain, projectRoot) ^ (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(terrain.textureTiling)) << 17);
        const GpuMesh* gpuMesh = ctx_.meshes().procedural("terrain:" + scene.uuidOf(e).toString(), key, [&] {
            return buildTerrainMesh(*terrainData(terrain, projectRoot), terrain.textureTiling);
        });
        if (gpuMesh == nullptr) continue;
        LandscapeEntry& entry = landscapeEntry(e, 1);
        entry.look.baseColor = terrain.grassColor;
        entry.look.roughness = 0.92f;
        entry.look.baseColorTexture = terrain.texture;
        auto channel = [](float c) { return static_cast<std::uint32_t>(std::lround(std::sqrt(std::clamp(c, 0.0f, 1.0f)) * 255.0f)); };
        const std::uint32_t rock = channel(terrain.rockColor.r) | (channel(terrain.rockColor.g) << 8) | (channel(terrain.rockColor.b) << 16);
        candidates.push_back({e, &entry.look, &world.matrix, gpuMesh, nullptr, 3.0f, static_cast<float>(rock)});
    }
    // Foliage: one candidate per copy; consecutive objects of one mesh become instanced draws.
    for (auto [e, foliage] : reg.view<FoliageComponent>().each()) {
        LandscapeEntry& entry = landscapeEntry(e, 2);
        const std::uint64_t key = foliageKey(scene, e, projectRoot);
        if (entry.key != key || entry.worlds.empty() != (foliage.count == 0)) {
            entry.key = key;
            entry.worlds = foliageInstances(scene, e, projectRoot);
        }
        entry.look.mesh = foliage.mesh;
        entry.look.meshAsset = foliage.meshAsset;
        entry.look.material = foliage.material;
        entry.look.baseColor = foliage.color;
        entry.look.roughness = 0.8f;
        const GpuMesh* gpuMesh = meshFor(entry.look, settings.assets, settings.blockOnAssets);
        if (gpuMesh == nullptr) continue;
        std::shared_ptr<const MaterialAsset> material;
        if (!foliage.material.empty() && settings.assets != nullptr) material = settings.assets->material(foliage.material);
        for (const glm::mat4& m : entry.worlds) candidates.push_back({e, &entry.look, &m, gpuMesh, material});
    }
    std::erase_if(landscape_, [&](const auto& item) { return item.second.lastFrame + 300 < frameCounter_; });
    for (auto [e, mesh, world] : reg.view<MeshRendererComponent, WorldTransformComponent>().each()) {
        if (!mesh.visible) continue;
        const GpuMesh* gpuMeshPtr = meshFor(mesh, settings.assets, settings.blockOnAssets);
        if (gpuMeshPtr == nullptr) continue;  // still loading, or unresolved (already reported)
        std::shared_ptr<const MaterialAsset> material;
        if (!mesh.material.empty() && settings.assets != nullptr) material = settings.assets->material(mesh.material);
        candidates.push_back({e, &mesh, &world.matrix, gpuMeshPtr, std::move(material)});
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
            // Skinned meshes are never culled: animated limbs leave the bind-pose bounds.
            p.visible = !settings.frustumCulling || c.gpuMesh->skinned() || frustum.intersectsAabb(p.center, p.extent);
            if (!p.visible && !anyShadows) continue;
            const MeshRendererComponent& mesh = *c.mesh;
            const MaterialAsset* mat = c.material.get();
            GpuObject& obj = p.object;
            obj.world = world;
            obj.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world))));
            if (mat != nullptr) {
                obj.baseColorMetallic = glm::vec4(mat->baseColor, mat->metallic);
                obj.emissiveRoughness = glm::vec4(mat->emissive * mat->emissiveStrength, mat->roughness);
            } else {
                obj.baseColorMetallic = glm::vec4(mesh.baseColor, mesh.metallic);
                obj.emissiveRoughness = glm::vec4(mesh.emissive * mesh.emissiveStrength, mesh.roughness);
            }
            const float pattern = c.pattern >= 0.0f ? c.pattern
                                                    : static_cast<float>(mat != nullptr ? mat->pattern : static_cast<std::int32_t>(mesh.pattern));
            const bool highlighted = c.entity == settings.selected ||
                                     std::ranges::find(settings.alsoSelected, c.entity) != settings.alsoSelected.end();
            obj.flags = glm::vec4(highlighted ? 1.0f : 0.0f, pattern, c.extra, 0.0f);
        }
    });

    // 3. Textures (serial: the cache may start uploads) and the draw lists, in scene order.
    objects.reserve(candidates.size());
    std::vector<glm::mat4> joints;  // skinning palettes of every skinned object, back to back
    // Objects grouped by mesh (unskinned first), so copies of a mesh sit next to each other.
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::ranges::stable_sort(order, std::less<>{}, [&](std::size_t i) {
        return std::pair(candidates[i].gpuMesh->skinned(), candidates[i].gpuMesh);
    });
    for (const std::size_t i : order) {
        Prepared& p = prepared[i];
        if (!p.visible) {
            ++stats.culledObjects;
            if (!anyShadows) continue;
        }
        GpuObject& obj = p.object;
        if (p.visible && settings.assets != nullptr) {  // casters only need depth, not materials
            const MeshRendererComponent& mesh = *candidates[i].mesh;
            const MaterialAsset* m = candidates[i].material.get();
            TextureCache& textures = ctx_.textures();
            const bool block = settings.blockOnAssets;
            const std::string& baseMap = m != nullptr ? m->baseColorTexture : mesh.baseColorTexture;
            const std::string& mrMap = m != nullptr ? m->metallicRoughnessTexture : mesh.metallicRoughnessTexture;
            const std::string& emissiveMap = m != nullptr ? m->emissiveTexture : mesh.emissiveTexture;
            const std::string& normalMap = m != nullptr ? m->normalTexture : mesh.normalTexture;
            obj.textures = glm::uvec4(textures.index(*settings.assets, baseMap, TextureColorSpace::Srgb, block),
                                      textures.index(*settings.assets, mrMap, TextureColorSpace::Linear, block),
                                      textures.index(*settings.assets, emissiveMap, TextureColorSpace::Srgb, block),
                                      textures.index(*settings.assets, normalMap, TextureColorSpace::Linear, block));
        }
        if (candidates[i].gpuMesh->skinned()) {
            obj.flags.z = static_cast<float>(joints.size());
            obj.flags.w = appendSkinPalette(scene, candidates[i].entity, *candidates[i].mesh, settings.assets, joints) ? 1.0f : 0.0f;
        }
        const Draw draw{candidates[i].gpuMesh, static_cast<std::uint32_t>(objects.size()), p.center, p.extent};
        if (p.visible) draws.push_back(draw);
        if (anyShadows) casters.push_back(draw);
        objects.push_back(obj);
    }
    // Unskinned first, then skinned (one pipeline switch); within each, grouped by mesh.
    auto byPipelineAndMesh = [](const Draw& d) { return std::pair(d.mesh->skinned(), d.mesh); };
    std::ranges::stable_sort(draws, std::less<>{}, byPipelineAndMesh);
    std::ranges::stable_sort(casters, std::less<>{}, byPipelineAndMesh);
    // Decals are objects too: world, inverse world (in normalMatrix), colour + opacity, texture.
    std::vector<std::uint32_t> decals;
    for (auto [e, decal, world] : reg.view<DecalComponent, WorldTransformComponent>().each()) {
        const glm::vec3 center(world.matrix[3]);
        const glm::mat3 m(world.matrix);
        const glm::vec3 halfExtent = (glm::abs(m[0]) + glm::abs(m[1]) + glm::abs(m[2])) * 0.5f;
        if (settings.frustumCulling && !frustum.intersectsAabb(center, halfExtent)) continue;
        GpuObject obj;
        obj.world = world.matrix;
        obj.normalMatrix = glm::inverse(world.matrix);
        obj.baseColorMetallic = glm::vec4(decal.color, std::clamp(decal.opacity, 0.0f, 1.0f));
        if (!decal.texture.empty() && settings.assets != nullptr) {
            obj.textures.x = ctx_.textures().index(*settings.assets, decal.texture, TextureColorSpace::Srgb, settings.blockOnAssets);
        }
        decals.push_back(static_cast<std::uint32_t>(objects.size()));
        objects.push_back(obj);
    }
    // Water surfaces: objects too (a unit grid scaled to the surface; wave settings in flags).
    std::vector<std::uint32_t> waters;
    const GpuMesh* waterGrid = nullptr;
    for (auto [e, water, world] : reg.view<WaterComponent, WorldTransformComponent>().each()) {
        const glm::mat4 surface = world.matrix * glm::scale(glm::mat4(1.0f), glm::vec3(water.width, 1.0f, water.length));
        const glm::vec3 center(surface[3]);
        const glm::mat3 m(surface);
        const glm::vec3 halfExtent = (glm::abs(m[0]) + glm::abs(m[2])) * 0.5f + glm::vec3(0.0f, water.waveHeight * 1.5f, 0.0f);
        if (settings.frustumCulling && !frustum.intersectsAabb(center, halfExtent)) continue;
        if (waterGrid == nullptr) waterGrid = ctx_.meshes().procedural("water:grid", 256, [] { return buildWaterGrid(256); });
        GpuObject obj;
        obj.world = surface;
        obj.baseColorMetallic = glm::vec4(water.shallowColor, 0.0f);
        obj.emissiveRoughness = glm::vec4(water.deepColor, 0.0f);
        obj.flags = glm::vec4(water.waveHeight, water.waveLength, water.waveSpeed, water.clarity);
        waters.push_back(static_cast<std::uint32_t>(objects.size()));
        objects.push_back(obj);
    }
    if (objects.empty()) objects.emplace_back();  // structured buffers may not be empty
    if (joints.empty()) joints.emplace_back(1.0f);
    frame.objects.write(gpu, std::as_bytes(std::span(objects)), DynamicBuffer::Kind::Structured, sizeof(GpuObject),
                        "FrameObjects");
    frame.joints.write(gpu, std::as_bytes(std::span(joints)), DynamicBuffer::Kind::Structured, sizeof(glm::mat4),
                       "FrameJoints");
    stats.visibleObjects = static_cast<std::uint32_t>(draws.size());
    for (const Draw& d : draws) {
        stats.triangles += d.mesh->indexCount / 3;
    }
    draws = instanced(draws);
    stats.drawCalls = static_cast<std::uint32_t>(draws.size()) * 2 + 2 + (settings.showGrid ? 1u : 0u);  // prepass + shading

    // ---- Render graph ----------------------------------------------------------------------
    const RGImage hdr = graph.createImage("SceneColorHDR", {ctx_.hdrFormat(), extent.width, extent.height});
    const RGImage depth = graph.createImage("SceneDepth", {ctx_.depthFormat(), extent.width, extent.height});
    nvrhi::IBindingSet* frameSet = frame.bindingSet(gpu, ctx_.frameLayout(), ctx_.samplers());
    nvrhi::IDescriptorTable* bindless = ctx_.bindless().table();
    // Every draw: a pipeline over the pass's framebuffer with set 0 and the bindless set 1.
    auto stateFor = [frameSet, bindless](nvrhi::IGraphicsPipeline* pipeline, const PassContext& ctx) {
        nvrhi::GraphicsState state;
        state.pipeline = pipeline;
        state.framebuffer = ctx.framebuffer;
        state.viewport = ctx.viewport();
        state.bindings = {frameSet, bindless};
        return state;
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
                cascadeCasters[c] = instanced(cascadeCasters[c]);
            }
        });
        std::size_t casterDraws = 0;
        for (const auto& list : cascadeCasters) casterDraws += list.size();
        stats.drawCalls += static_cast<std::uint32_t>(casterDraws);
        const bool parallelShadow = casterDraws >= settings.parallelRecordingMinDraws;
        stats.parallelRecording = stats.parallelRecording || parallelShadow;
        shadowMap = graph.createImage("SunShadowAtlas", {ctx_.depthFormat(), atlasSize, atlasSize});
        graph.addRasterPass(
            "SunShadow",
            [&](PassBuilder& b) {
                b.writeDepth(shadowMap, LoadOp::Clear, 1.0f);
                b.recordInParallel(parallelShadow);
            },
            [this, stateFor, tile, cascadeCasters = std::move(cascadeCasters)](PassContext& ctx) {
                // One cascade per command list when recorded in parallel.
                ctx.recordParallel(kShadowCascades, 1, [&](nvrhi::ICommandList* cmd, std::size_t begin, std::size_t end) {
                    for (std::size_t c = begin; c < end; ++c) {
                        const float x = static_cast<float>((c % 2) * tile);
                        const float y = static_cast<float>((c / 2) * tile);
                        nvrhi::GraphicsState state = stateFor(shadow_, ctx);
                        state.viewport = nvrhi::ViewportState().addViewportAndScissorRect(
                            nvrhi::Viewport(x, x + static_cast<float>(tile), y, y + static_cast<float>(tile), 0.0f, 1.0f));
                        const GpuMesh* mesh = nullptr;
                        for (const Draw& d : cascadeCasters[c]) {
                            if (d.mesh != mesh) {
                                mesh = d.mesh;
                                state.pipeline = mesh->skinned() ? shadowSkinned_ : shadow_;
                                bindMesh(state, *mesh);
                                cmd->setGraphicsState(state);
                            }
                            PushConstants pc;
                            pc.index0 = d.object;
                            pc.index1 = static_cast<std::uint32_t>(c);
                            push(cmd, pc);
                            cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh->indexCount).setInstanceCount(d.count));
                        }
                    }
                });
            });
    }

    RGImage localAtlas{};
    if (localShadows) {
        // Casters per tile: inside the light's range and the tile's frustum.
        std::vector<std::vector<Draw>> tileCasters(localTiles.size());
        JobSystem::global().parallelFor(localTiles.size(), 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t t = begin; t < end; ++t) {
                const LocalTile& localTile = localTiles[t];
                const Frustum frustumOfTile = Frustum::fromViewProjection(localTile.viewProjection);
                for (const Draw& d : casters) {
                    if (glm::length(d.center - localTile.lightPosition) - glm::length(d.extent) > localTile.range) continue;
                    if (frustumOfTile.intersectsAabb(d.center, d.extent)) tileCasters[t].push_back(d);
                }
                tileCasters[t] = instanced(tileCasters[t]);
            }
        });
        for (const auto& list : tileCasters) stats.drawCalls += static_cast<std::uint32_t>(list.size());
        localAtlas = graph.createImage("LocalShadowAtlas", {ctx_.depthFormat(), localAtlasSize, localAtlasSize});
        const std::uint32_t tileSize = localAtlasSize / 4;
        graph.addRasterPass(
            "LocalShadows", [&](PassBuilder& b) { b.writeDepth(localAtlas, LoadOp::Clear, 1.0f); },
            [this, stateFor, tileSize, tileCasters = std::move(tileCasters)](PassContext& ctx) {
                for (std::size_t t = 0; t < tileCasters.size(); ++t) {
                    const float x = static_cast<float>((t % 4) * tileSize);
                    const float y = static_cast<float>((t / 4) * tileSize);
                    nvrhi::GraphicsState state = stateFor(shadow_, ctx);
                    state.viewport = nvrhi::ViewportState().addViewportAndScissorRect(
                        nvrhi::Viewport(x, x + static_cast<float>(tileSize), y, y + static_cast<float>(tileSize), 0.0f, 1.0f));
                    const GpuMesh* mesh = nullptr;
                    for (const Draw& d : tileCasters[t]) {
                        if (d.mesh != mesh) {
                            mesh = d.mesh;
                            state.pipeline = mesh->skinned() ? shadowSkinned_ : shadow_;
                            bindMesh(state, *mesh);
                            ctx.cmd->setGraphicsState(state);
                        }
                        PushConstants pc;
                        pc.index0 = d.object;
                        pc.index1 = static_cast<std::uint32_t>(t);
                        pc.flags = 1u;  // a local shadow tile
                        push(ctx.cmd, pc);
                        ctx.cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh->indexCount).setInstanceCount(d.count));
                    }
                }
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
        [this, stateFor, draws](PassContext& ctx) {  // a copy: runs after build() returns
            if (draws.empty()) return;
            ctx.recordParallel(draws.size(), 64, [&](nvrhi::ICommandList* cmd, std::size_t begin, std::size_t end) {
                nvrhi::GraphicsState state = stateFor(depthPrepass_, ctx);
                const GpuMesh* mesh = nullptr;
                for (std::size_t i = begin; i < end; ++i) {
                    const Draw& d = draws[i];
                    if (d.mesh != mesh) {
                        mesh = d.mesh;
                        state.pipeline = mesh->skinned() ? depthPrepassSkinned_ : depthPrepass_;
                        bindMesh(state, *mesh);
                        cmd->setGraphicsState(state);
                    }
                    PushConstants pc;
                    pc.index0 = d.object;
                    push(cmd, pc);
                    cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh->indexCount).setInstanceCount(d.count));
                }
            });
        });

    RGImage ao{};
    const bool ambientOcclusion = settings.ambientOcclusion && !draws.empty() && extent.width >= 8 && extent.height >= 8;
    if (ambientOcclusion) {
        const RGImage aoRaw = graph.createImage("SSAORaw", {kAoFormat, std::max(extent.width / 2, 1u),
                                                              std::max(extent.height / 2, 1u)});
        ao = graph.createImage("SSAO", {kAoFormat, extent.width, extent.height});
        const std::uint32_t linear = ctx_.linearClampSampler();
        graph.addRasterPass(
            "SSAO",
            [&](PassBuilder& b) {
                b.writeColor(aoRaw, LoadOp::DontCare);
                b.sample(depth);
            },
            [this, stateFor, depth, radius = settings.aoRadius, intensity = settings.aoIntensity](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(depth);
                pc.param0 = radius;
                pc.param1 = intensity;
                pc.param2 = 0.02f;
                fullscreen(ctx.cmd, stateFor(ssao_, ctx), pc);
            });
        graph.addRasterPass(
            "SSAOBlur",
            [&](PassBuilder& b) {
                b.writeColor(ao, LoadOp::DontCare);
                b.sample(aoRaw);
                b.sample(depth);
            },
            [this, stateFor, depth, aoRaw, linear](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(depth);
                pc.index1 = ctx.bindlessIndex(aoRaw);
                pc.index2 = linear;
                fullscreen(ctx.cmd, stateFor(ssaoBlur_, ctx), pc);
            });
        stats.ambientOcclusion = true;
    }

    graph.addRasterPass(
        "Sky", [&](PassBuilder& b) { b.writeColor(hdr, LoadOp::DontCare); },
        [this, stateFor](PassContext& ctx) { fullscreen(ctx.cmd, stateFor(sky_, ctx), PushConstants{}); });

    const bool parallelOpaque = draws.size() >= settings.parallelRecordingMinDraws;
    stats.parallelRecording = stats.parallelRecording || parallelOpaque;
    graph.addRasterPass(
        "Opaque",
        [&](PassBuilder& b) {
            b.writeColor(hdr, LoadOp::Load);
            b.readDepth(depth);  // from the prepass; the opaque pass only tests against it
            if (shadows) b.sample(shadowMap);
            if (localShadows) b.sample(localAtlas);
            if (ambientOcclusion) b.sample(ao);
            b.recordInParallel(parallelOpaque);
        },
        [this, stateFor, shadows, shadowMap, localShadows, localAtlas, ambientOcclusion, ao,
         debugCascades = settings.debugCascades,
         draws = std::move(draws)](PassContext& ctx) {
            if (draws.empty()) return;
            const std::uint32_t shadowIndex = shadows ? ctx.bindlessIndex(shadowMap) : 0u;
            const std::uint32_t aoIndex = ambientOcclusion ? ctx.bindlessIndex(ao) : 0u;
            const float localAtlasIndex = std::bit_cast<float>(localShadows ? ctx.bindlessIndex(localAtlas) : 0u);
            const std::uint32_t flags =
                (shadows ? 1u : 0u) | (shadows && debugCascades ? 2u : 0u) | (ambientOcclusion ? 4u : 0u);
            // Draws are sorted by mesh, so each chunk rebinds buffers only where meshes change.
            ctx.recordParallel(draws.size(), 64, [&](nvrhi::ICommandList* cmd, std::size_t begin, std::size_t end) {
                nvrhi::GraphicsState state = stateFor(opaque_, ctx);
                const GpuMesh* mesh = nullptr;
                for (std::size_t i = begin; i < end; ++i) {
                    const Draw& d = draws[i];
                    if (d.mesh != mesh) {
                        mesh = d.mesh;
                        state.pipeline = mesh->skinned() ? opaqueSkinned_ : opaque_;
                        bindMesh(state, *mesh);
                        cmd->setGraphicsState(state);
                    }
                    PushConstants pc;
                    pc.index0 = d.object;
                    pc.index1 = shadowIndex;
                    pc.index2 = aoIndex;
                    pc.flags = flags;
                    pc.param0 = localAtlasIndex;
                    push(cmd, pc);
                    cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh->indexCount).setInstanceCount(d.count));
                }
            });
        });

    if (!decals.empty()) {
        const GpuMesh* box = &ctx_.meshes().primitive(MeshPrimitive::Cube);
        const std::uint32_t sampler = ctx_.linearClampSampler();
        graph.addRasterPass(
            "Decals",
            [&](PassBuilder& b) {
                b.writeColor(hdr, LoadOp::Load);
                b.sample(depth);
            },
            [this, stateFor, box, depth, sampler, decals](PassContext& ctx) {
                nvrhi::GraphicsState state = stateFor(decal_, ctx);
                bindMesh(state, *box);
                ctx.cmd->setGraphicsState(state);
                for (const std::uint32_t object : decals) {
                    PushConstants pc;
                    pc.index0 = object;
                    pc.index1 = ctx.bindlessIndex(depth);
                    pc.index2 = sampler;
                    push(ctx.cmd, pc);
                    ctx.cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(box->indexCount));
                }
            });
        stats.drawCalls += static_cast<std::uint32_t>(decals.size());
    }

    if (!waters.empty()) {
        const std::uint32_t sampler = ctx_.linearClampSampler();
        graph.addRasterPass(
            "Water",
            [&](PassBuilder& b) {
                b.writeColor(hdr, LoadOp::Load);
                b.sample(depth);
            },
            [this, stateFor, waterGrid, depth, sampler, waters](PassContext& ctx) {
                nvrhi::GraphicsState state = stateFor(water_, ctx);
                bindMesh(state, *waterGrid);
                ctx.cmd->setGraphicsState(state);
                for (const std::uint32_t object : waters) {
                    PushConstants pc;
                    pc.index0 = object;
                    pc.index1 = ctx.bindlessIndex(depth);
                    pc.index2 = sampler;
                    push(ctx.cmd, pc);
                    ctx.cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(waterGrid->indexCount));
                }
            });
        stats.drawCalls += static_cast<std::uint32_t>(waters.size());
    }

    if (settings.showGrid) {
        graph.addRasterPass(
            "Grid",
            [&](PassBuilder& b) {
                b.writeColor(hdr, LoadOp::Load);
                b.readDepth(depth);
            },
            [this, stateFor](PassContext& ctx) { fullscreen(ctx.cmd, stateFor(grid_, ctx), PushConstants{}); });
    }

    // ---- Particles: camera-facing quads, alpha-blended ones far to near, then additive ones.
    {
        struct Quad {
            glm::vec3 center;
            float size;
            glm::vec4 color;
            float depth;
        };
        std::vector<Quad> alpha;
        std::vector<Quad> additive;
        const glm::vec3 right(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
        const glm::vec3 up(camera.view[0][1], camera.view[1][1], camera.view[2][1]);
        const glm::vec3 forward(-camera.view[0][2], -camera.view[1][2], -camera.view[2][2]);
        auto linear = [](const glm::vec3& srgb) { return glm::pow(glm::max(srgb, glm::vec3(0.0f)), glm::vec3(2.2f)); };
        for (auto [e, emitter, state] : reg.view<ParticleEmitterComponent, ParticleStateComponent>().each()) {
            if (state.particles.empty()) continue;
            const auto* worldTransform = reg.try_get<WorldTransformComponent>(e);
            const glm::mat4 world = worldTransform != nullptr ? worldTransform->matrix : glm::mat4(1.0f);
            const glm::vec3 c0 = linear(emitter.startColor) * emitter.intensity;
            const glm::vec3 c1 = linear(emitter.endColor) * emitter.intensity;
            auto& out = emitter.blend == ParticleBlend::Additive ? additive : alpha;
            for (const auto& p : state.particles) {
                const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
                const glm::vec3 position = emitter.worldSpace ? p.position : glm::vec3(world * glm::vec4(p.position, 1.0f));
                const float a = glm::mix(emitter.startAlpha, emitter.endAlpha, t);
                if (a <= 0.0f) continue;
                out.push_back({position, glm::mix(emitter.startSize, emitter.endSize, t) * 0.5f,
                               glm::vec4(glm::mix(c0, c1, t), a), glm::dot(position - camera.position, forward)});
            }
        }
        std::ranges::sort(alpha, std::greater<>{}, &Quad::depth);  // far first
        const std::size_t alphaCount = alpha.size();
        const std::size_t total = alphaCount + additive.size();
        stats.particles = static_cast<std::uint32_t>(total);
        if (total > 0) {
            std::vector<ParticleVertex> vertices;
            vertices.reserve(total * 6);
            auto emit = [&](const Quad& q) {
                const glm::vec3 r = right * q.size;
                const glm::vec3 u = up * q.size;
                const ParticleVertex v00{q.center - r - u, {-1.0f, -1.0f}, q.color};
                const ParticleVertex v10{q.center + r - u, {1.0f, -1.0f}, q.color};
                const ParticleVertex v11{q.center + r + u, {1.0f, 1.0f}, q.color};
                const ParticleVertex v01{q.center - r + u, {-1.0f, 1.0f}, q.color};
                vertices.insert(vertices.end(), {v00, v10, v11, v00, v11, v01});
            };
            for (const Quad& q : alpha) emit(q);
            for (const Quad& q : additive) emit(q);
            nvrhi::IBuffer* vb = frame.particleVertices.write(gpu, std::as_bytes(std::span(vertices)),
                                                              DynamicBuffer::Kind::Vertex, sizeof(ParticleVertex),
                                                              "ParticleVertices");
            const auto alphaVertices = static_cast<std::uint32_t>(alphaCount * 6);
            const auto additiveVertices = static_cast<std::uint32_t>((total - alphaCount) * 6);
            graph.addRasterPass(
                "Particles",
                [&](PassBuilder& b) {
                    b.writeColor(hdr, LoadOp::Load);
                    b.readDepth(depth);
                },
                [this, stateFor, vb, alphaVertices, additiveVertices](PassContext& ctx) {
                    nvrhi::GraphicsState state = stateFor(particlesAlpha_, ctx);
                    state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(vb).setSlot(0)};
                    PushConstants pc;
                    if (alphaVertices > 0) {
                        ctx.cmd->setGraphicsState(state);
                        pc.flags = 0u;
                        push(ctx.cmd, pc);
                        ctx.cmd->draw(nvrhi::DrawArguments().setVertexCount(alphaVertices));
                    }
                    if (additiveVertices > 0) {
                        state.pipeline = particlesAdditive_;
                        ctx.cmd->setGraphicsState(state);
                        pc.flags = 1u;
                        push(ctx.cmd, pc);
                        ctx.cmd->draw(nvrhi::DrawArguments().setVertexCount(additiveVertices).setStartVertexLocation(alphaVertices));
                    }
                });
        }
    }

    // ---- Sun shafts (half resolution, added onto the HDR image), then depth of field.
    const std::uint32_t linearSampler = ctx_.linearClampSampler();
    if (fc.sunShafts.z > 0.0f && fc.sunShafts.w > 0.0f) {
        const RGImage shaftsImage = graph.createImage("SunShafts", {ctx_.hdrFormat(), std::max(extent.width / 2, 1u),
                                                                    std::max(extent.height / 2, 1u)});
        graph.addRasterPass(
            "SunShafts",
            [&](PassBuilder& b) {
                b.writeColor(shaftsImage, LoadOp::DontCare);
                b.sample(hdr);
                b.sample(depth);
            },
            [this, stateFor, hdr, depth, linearSampler](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(hdr);
                pc.index1 = ctx.bindlessIndex(depth);
                pc.index2 = linearSampler;
                fullscreen(ctx.cmd, stateFor(shafts_, ctx), pc);
            });
        graph.addRasterPass(
            "SunShaftsAdd",
            [&](PassBuilder& b) {
                b.writeColor(hdr, LoadOp::Load);
                b.sample(shaftsImage);
            },
            [this, stateFor, shaftsImage, linearSampler](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(shaftsImage);
                pc.index2 = linearSampler;
                fullscreen(ctx.cmd, stateFor(shaftsAdd_, ctx), pc);
            });
    }
    RGImage sceneColor = hdr;
    if (fc.depthOfField.z > 0.5f) {
        sceneColor = graph.createImage("DepthOfField", {ctx_.hdrFormat(), extent.width, extent.height});
        graph.addRasterPass(
            "DepthOfField",
            [&](PassBuilder& b) {
                b.writeColor(sceneColor, LoadOp::DontCare);
                b.sample(hdr);
                b.sample(depth);
            },
            [this, stateFor, hdr, depth, linearSampler](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(hdr);
                pc.index1 = ctx.bindlessIndex(depth);
                pc.index2 = linearSampler;
                fullscreen(ctx.cmd, stateFor(depthOfField_, ctx), pc);
            });
    }

    // ---- Bloom: downsample chain, then additive tent upsample back to the half-res level.
    const std::uint32_t sampler = ctx_.linearClampSampler();
    std::vector<RGImage> bloomChain;
    if (settings.bloom && env.bloomIntensity > 0.0f) {
        std::uint32_t w = extent.width, h = extent.height;
        while (bloomChain.size() < 6 && w >= 16 && h >= 16) {
            w /= 2;
            h /= 2;
            bloomChain.push_back(graph.createImage("Bloom" + std::to_string(bloomChain.size()), {ctx_.hdrFormat(), w, h}));
        }
    }
    nvrhi::IGraphicsPipeline* bloomDown = bloomDown_;
    nvrhi::IGraphicsPipeline* bloomUp = bloomUp_;
    for (std::size_t i = 0; i < bloomChain.size(); ++i) {
        const RGImage source = i == 0 ? sceneColor : bloomChain[i - 1];
        const RGImage target = bloomChain[i];
        graph.addRasterPass(
            "BloomDown" + std::to_string(i),
            [&](PassBuilder& b) {
                b.sample(source);
                b.writeColor(target, LoadOp::DontCare);
            },
            [=](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(source);
                pc.index1 = sampler;
                pc.flags = i == 0 ? 1u : 0u;
                fullscreen(ctx.cmd, stateFor(bloomDown, ctx), pc);
            });
    }
    for (std::size_t i = bloomChain.size(); i-- > 1;) {
        const RGImage source = bloomChain[i];
        const RGImage target = bloomChain[i - 1];
        const float radius = 1.0f / static_cast<float>(std::max(extent.width >> (i + 1), 1u));
        graph.addRasterPass(
            "BloomUp" + std::to_string(i),
            [&](PassBuilder& b) {
                b.sample(source);
                b.writeColor(target, LoadOp::Load);
            },
            [=](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(source);
                pc.index1 = sampler;
                pc.param0 = radius;
                fullscreen(ctx.cmd, stateFor(bloomUp, ctx), pc);
            });
    }
    stats.drawCalls += static_cast<std::uint32_t>(bloomChain.empty() ? 0 : bloomChain.size() * 2 - 1);

    // With FXAA the tonemapper writes a gamma-encoded LDR image (luma in alpha) that FXAA then
    // resolves into the output; without, it writes the output directly.
    const bool fxaa = settings.antiAliasing;
    // Half floats: the gamma-encoded image is quantised only once, when FXAA writes the output.
    constexpr nvrhi::Format kLdrFormat = nvrhi::Format::RGBA16_FLOAT;
    const RGImage ldr = fxaa ? graph.createImage("LDR", {kLdrFormat, extent.width, extent.height}) : output;
    const nvrhi::Format tonemapFormat = fxaa ? kLdrFormat : outputFormat;
    PipelineDesc fullscreenDesc;
    fullscreenDesc.vertexShader = "tonemap.vert";
    fullscreenDesc.pixelShader = "tonemap.frag";
    fullscreenDesc.name = "Tonemap";
    nvrhi::IGraphicsPipeline* tonemap = outputPipeline(tonemap_, fullscreenDesc, tonemapFormat);
    const bool shaderSrgb = !isSrgbFormat(tonemapFormat);
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
            b.sample(sceneColor);
            if (haveBloom) b.sample(bloomResult);
            b.writeColor(ldr, LoadOp::DontCare);
        },
        [=](PassContext& ctx) {
            PushConstants pc;
            pc.index0 = ctx.bindlessIndex(sceneColor);
            pc.index1 = sampler;
            pc.index2 = haveBloom ? ctx.bindlessIndex(bloomResult) : 0u;
            pc.flags = (shaderSrgb ? 1u : 0u) | (haveBloom ? 2u : 0u) | (fxaa ? 4u : 0u);
            pc.param0 = exposure;
            pc.param1 = bloomStrength;
            pc.param2 = vignette;
            pc.param3 = bloomNormalisation;
            fullscreen(ctx.cmd, stateFor(tonemap, ctx), pc);
        });
    if (fxaa) {
        PipelineDesc fxaaDesc;
        fxaaDesc.vertexShader = "fxaa.vert";
        fxaaDesc.pixelShader = "fxaa.frag";
        fxaaDesc.name = "FXAA";
        nvrhi::IGraphicsPipeline* resolve = outputPipeline(fxaa_, fxaaDesc, outputFormat);
        const bool outputSrgb = isSrgbFormat(outputFormat);
        graph.addRasterPass(
            "FXAA",
            [&](PassBuilder& b) {
                b.sample(ldr);
                b.writeColor(output, LoadOp::DontCare);
            },
            [=](PassContext& ctx) {
                PushConstants pc;
                pc.index0 = ctx.bindlessIndex(ldr);
                pc.index1 = sampler;
                pc.flags = outputSrgb ? 1u : 0u;
                fullscreen(ctx.cmd, stateFor(resolve, ctx), pc);
            });
        stats.drawCalls += 1;
    }

    // ---- Overlay (gizmos): unlit, alpha blended, on top of the final image.
    // Debug shapes from behaviours join the overlay: thin solids, about 2 px wide on screen.
    OverlayGeometry debugGeometry;
    if (settings.debug != nullptr && !settings.debug->empty()) {
        const glm::vec3 forward(-camera.view[0][2], -camera.view[1][2], -camera.view[2][2]);
        OverlayBuilder builder(debugGeometry, forward);
        const float pixel = 2.0f * std::tan(camera.fovYRadians * 0.5f) / std::max(1.0f, static_cast<float>(extent.height));
        auto width = [&](const glm::vec3& at) { return std::max(0.002f, glm::length(at - camera.position) * pixel); };
        for (const DebugDraw::Line& l : settings.debug->lines) {
            if (glm::length(l.to - l.from) > 1e-5f) builder.cylinder(l.from, l.to, width((l.from + l.to) * 0.5f), l.color, 6);
        }
        for (const DebugDraw::Box& b : settings.debug->boxes) {
            const glm::vec3 axes[3] = {{b.halfExtent.x, 0, 0}, {0, b.halfExtent.y, 0}, {0, 0, b.halfExtent.z}};
            builder.wireBox(b.center, axes, width(b.center), b.color);
        }
        for (const DebugDraw::Sphere& s : settings.debug->spheres) {
            for (const glm::vec3 axis : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)}) {
                builder.torus(s.center, axis, s.radius, width(s.center), s.color, 40, 4);
            }
        }
    }
    std::vector<OverlayVertex> combinedOverlay;
    if (!debugGeometry.empty()) {
        if (settings.overlay != nullptr) combinedOverlay = settings.overlay->vertices;
        combinedOverlay.insert(combinedOverlay.end(), debugGeometry.vertices.begin(), debugGeometry.vertices.end());
    }
    const std::vector<OverlayVertex>* overlayVertices =
        !combinedOverlay.empty() ? &combinedOverlay : settings.overlay != nullptr ? &settings.overlay->vertices : nullptr;
    if (overlayVertices != nullptr && !overlayVertices->empty()) {
        const auto& vertices = *overlayVertices;
        nvrhi::IBuffer* vb = frame.overlayVertices.write(gpu, std::as_bytes(std::span(vertices)), DynamicBuffer::Kind::Vertex,
                                                         sizeof(OverlayVertex), "OverlayVertices");
        PipelineDesc overlayDesc;
        overlayDesc.name = "Overlay";
        overlayDesc.vertexShader = "overlay.vert";
        overlayDesc.pixelShader = "overlay.frag";
        overlayDesc.input = VertexInput::Overlay;
        overlayDesc.blend = BlendMode::Alpha;
        nvrhi::IGraphicsPipeline* overlay = outputPipeline(overlay_, overlayDesc, outputFormat);
        const auto count = static_cast<std::uint32_t>(vertices.size());
        graph.addRasterPass(
            "Overlay", [&](PassBuilder& b) { b.writeColor(output, LoadOp::Load); },
            [=](PassContext& ctx) {
                nvrhi::GraphicsState state = stateFor(overlay, ctx);
                state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(vb).setSlot(0)};
                ctx.cmd->setGraphicsState(state);
                PushConstants pc;
                pc.flags = isSrgbFormat(outputFormat) ? 0u : 1u;  // the overlay draws into the output itself
                push(ctx.cmd, pc);
                ctx.cmd->draw(nvrhi::DrawArguments().setVertexCount(count));
            });
        ++stats.drawCalls;
    }
    // ---- HUD: the game's text and rectangles, on top of everything.
    if (settings.hud != nullptr && !settings.hud->empty()) {
        if (!hudFont_) {
            hudFont_ = std::make_unique<HudFont>();
            const auto& atlas = hudFont_->atlas();
            const std::span<const std::byte> bytes = std::as_bytes(std::span(atlas));
            hudAtlas_ = ctx_.createTexture(std::span(&bytes, 1), hudFont_->atlasWidth(), hudFont_->atlasHeight(),
                                           nvrhi::Format::R8_UNORM, "HudFontAtlas");
            hudAtlasIndex_ = ctx_.bindless().registerTexture(hudAtlas_);
        }
        std::vector<HudVertex> vertices;
        hudFont_->layout(*settings.hud, vertices);
        // The canvas is laid out for its screen size; scale it to this target.
        const glm::vec2 scale(static_cast<float>(extent.width) / std::max(settings.hud->screenSize.x, 1.0f),
                              static_cast<float>(extent.height) / std::max(settings.hud->screenSize.y, 1.0f));
        for (HudVertex& v : vertices) v.position *= scale;
        nvrhi::IBuffer* vb = frame.hudVertices.write(gpu, std::as_bytes(std::span(vertices)), DynamicBuffer::Kind::Vertex,
                                                     sizeof(HudVertex), "HudVertices");
        PipelineDesc hudDesc;
        hudDesc.name = "HUD";
        hudDesc.vertexShader = "hud.vert";
        hudDesc.pixelShader = "hud.frag";
        hudDesc.input = VertexInput::Hud;
        hudDesc.blend = BlendMode::Alpha;
        nvrhi::IGraphicsPipeline* hud = outputPipeline(hud_, hudDesc, outputFormat);
        const auto count = static_cast<std::uint32_t>(vertices.size());
        const std::uint32_t atlas = hudAtlasIndex_;
        const bool unorm = !isSrgbFormat(outputFormat);
        graph.addRasterPass(
            "HUD", [&](PassBuilder& b) { b.writeColor(output, LoadOp::Load); },
            [=](PassContext& ctx) {
                nvrhi::GraphicsState state = stateFor(hud, ctx);
                state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(vb).setSlot(0)};
                ctx.cmd->setGraphicsState(state);
                PushConstants pc;
                pc.index0 = atlas;
                pc.index1 = sampler;
                pc.flags = unorm ? 1u : 0u;
                push(ctx.cmd, pc);
                ctx.cmd->draw(nvrhi::DrawArguments().setVertexCount(count));
            });
        ++stats.drawCalls;
    }
    return stats;
}

} // namespace lumora::render
