#pragma once

#include "renderer/HudRenderer.hpp"
#include "scene/DebugDraw.hpp"

#include "renderer/GpuTypes.hpp"
#include "renderer/Overlay.hpp"
#include "renderer/RenderGraph.hpp"
#include "scene/Camera.hpp"
#include "scene/Components.hpp"

#include <entt/entity/entity.hpp>

#include <array>
#include <unordered_map>

namespace lumora {
class AssetLibrary;
class Scene;
}

namespace lumora::render {

class RenderContext;
struct FrameResources;

struct RenderSettings {
    bool showGrid = false;
    float gridCellSize = 1.0f;
    float gridFadeDistance = 150.0f;
    bool frustumCulling = true;
    bool shadows = true;
    bool bloom = true;
    float shadowDistance = 50.0f;          // metres covered by the sun shadow cascades
    std::uint32_t shadowResolution = 2048; // per cascade; the atlas holds 2x2 cascades
    float cascadeSplitLambda = 0.8f;       // 0 = uniform cascade splits, 1 = logarithmic
    bool debugCascades = false;           // tint surfaces by the shadow cascade they sample
    entt::entity selected = entt::null;
    std::vector<entt::entity> alsoSelected;  // the rest of a multi-selection (outlined too)
    const OverlayGeometry* overlay = nullptr;  // drawn on top after tone mapping (gizmos)
    const HudCanvas* hud = nullptr;            // the game's 2D overlay (text, rectangles), drawn last
    const DebugDraw* debug = nullptr;          // behaviours' debug shapes, drawn on top like the overlay
    AssetLibrary* assets = nullptr;            // resolves MeshRenderer::meshAsset; null = primitives only
    /// Wait for assets instead of skipping/untexturing them while they load (captures, headless).
    bool blockOnAssets = false;
    /// Passes with at least this many draws record on several threads (secondary command
    /// buffers); fewer are cheaper to record on one. 0 = always, UINT32_MAX = never.
    std::uint32_t parallelRecordingMinDraws = 256;
    /// Screen-space ambient occlusion: darkens ambient light where geometry meets.
    bool ambientOcclusion = true;
    float aoRadius = 0.6f;     // metres
    float aoIntensity = 1.6f;  // exponent applied to the visibility
    /// FXAA on the final image: smooths jagged edges.
    bool antiAliasing = true;
};

struct SceneDrawStats {
    std::uint32_t drawCalls = 0;
    std::uint32_t triangles = 0;
    std::uint32_t visibleObjects = 0;
    std::uint32_t culledObjects = 0;
    std::uint32_t lights = 0;
    std::uint32_t particles = 0;
    bool parallelRecording = false;  // a pass was set up to record on several threads
    bool shadowsRendered = false;
    bool environmentLit = false;  // an HDR environment lit the frame
    bool ambientOcclusion = false;
};

/// Orthographic sun transform covering the camera frustum up to `distance` (one cascade),
/// snapped to shadow-map texels so the shadows do not shimmer as the camera moves.
struct ShadowFit {
    glm::mat4 viewProjection{1.0f};
    float radius = 0.0f;
};
[[nodiscard]] ShadowFit fitSunShadow(const CameraView& camera, const glm::vec3& lightDirection, float distance,
                                     std::uint32_t resolution);
/// View distance at which each shadow cascade ends ("practical" split scheme: a blend of
/// logarithmic and uniform splits). The last split is `distance`.
[[nodiscard]] std::array<float, kShadowCascades> cascadeSplits(float nearPlane, float distance, float lambda);

/// Six-plane (five for infinite reversed-Z) frustum extracted from a view-projection matrix.
struct Frustum {
    glm::vec4 planes[5];
    [[nodiscard]] static Frustum fromViewProjection(const glm::mat4& viewProjection);
    [[nodiscard]] bool intersectsAabb(const glm::vec3& center, const glm::vec3& extents) const;
};

/// Translates a Scene into render-graph passes: Sky -> Opaque (PBR forward) -> Grid -> Tonemap.
class SceneRenderer {
public:
    explicit SceneRenderer(RenderContext& context);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    SceneDrawStats build(RenderGraph& graph, FrameResources& frame, const Scene& scene, const CameraView& camera,
                         const RenderSettings& settings, RGImage output, nvrhi::Format outputFormat, VkExtent2D extent,
                         double timeSeconds);

private:
    [[nodiscard]] const struct GpuMesh* meshFor(const MeshRendererComponent& mesh, AssetLibrary* assets, bool block);
    /// Pipelines that draw into the output image, one per output format.
    nvrhi::IGraphicsPipeline* outputPipeline(std::unordered_map<int, nvrhi::GraphicsPipelineHandle>& cache,
                                             const PipelineDesc& desc, nvrhi::Format format);

    RenderContext& ctx_;
    nvrhi::GraphicsPipelineHandle sky_;
    nvrhi::GraphicsPipelineHandle opaque_;
    nvrhi::GraphicsPipelineHandle opaqueSkinned_;
    nvrhi::GraphicsPipelineHandle grid_;
    nvrhi::GraphicsPipelineHandle particlesAlpha_;
    nvrhi::GraphicsPipelineHandle particlesAdditive_;
    nvrhi::GraphicsPipelineHandle shadow_;
    nvrhi::GraphicsPipelineHandle shadowSkinned_;
    nvrhi::GraphicsPipelineHandle depthPrepass_;
    nvrhi::GraphicsPipelineHandle depthPrepassSkinned_;
    nvrhi::GraphicsPipelineHandle ssao_;
    nvrhi::GraphicsPipelineHandle ssaoBlur_;
    nvrhi::GraphicsPipelineHandle bloomDown_;
    nvrhi::GraphicsPipelineHandle bloomUp_;
    nvrhi::GraphicsPipelineHandle decal_;
    nvrhi::GraphicsPipelineHandle depthOfField_;
    nvrhi::GraphicsPipelineHandle shafts_;
    nvrhi::GraphicsPipelineHandle shaftsAdd_;
    std::unordered_map<int, nvrhi::GraphicsPipelineHandle> tonemap_;
    std::unordered_map<int, nvrhi::GraphicsPipelineHandle> overlay_;
    std::unordered_map<int, nvrhi::GraphicsPipelineHandle> fxaa_;
    std::unordered_map<int, nvrhi::GraphicsPipelineHandle> hud_;
    std::unique_ptr<HudFont> hudFont_;  // built on first use
    nvrhi::TextureHandle hudAtlas_;
    std::uint32_t hudAtlasIndex_ = UINT32_MAX;
};

} // namespace lumora::render
