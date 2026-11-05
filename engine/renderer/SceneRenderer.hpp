#pragma once

#include "renderer/GpuTypes.hpp"
#include "renderer/Overlay.hpp"
#include "renderer/RenderGraph.hpp"
#include "renderer/vulkan/Resources.hpp"
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
    const OverlayGeometry* overlay = nullptr;  // drawn on top after tone mapping (gizmos)
    AssetLibrary* assets = nullptr;            // resolves MeshRenderer::meshAsset; null = primitives only
    /// Wait for assets instead of skipping/untexturing them while they load (captures, headless).
    bool blockOnAssets = false;
};

struct SceneDrawStats {
    std::uint32_t drawCalls = 0;
    std::uint32_t triangles = 0;
    std::uint32_t visibleObjects = 0;
    std::uint32_t culledObjects = 0;
    std::uint32_t lights = 0;
    bool shadowsRendered = false;
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

    SceneDrawStats build(RenderGraph& graph, FrameResources& frame, const Scene& scene, const CameraView& camera,
                         const RenderSettings& settings, RGImage output, VkFormat outputFormat, VkExtent2D extent,
                         double timeSeconds);

private:
    void ensureObjectCapacity(FrameResources& frame, std::uint32_t count);
    [[nodiscard]] const struct GpuMesh* meshFor(const MeshRendererComponent& mesh, AssetLibrary* assets, bool block);
    VkPipeline tonemapPipeline(VkFormat outputFormat);
    VkPipeline overlayPipeline(VkFormat outputFormat);

    RenderContext& ctx_;
    rhi::UniquePipeline sky_;
    rhi::UniquePipeline opaque_;
    rhi::UniquePipeline grid_;
    rhi::UniquePipeline shadow_;
    std::unordered_map<int, rhi::UniquePipeline> bloom_;  // 0 = downsample, 1 = upsample
    std::unordered_map<VkFormat, rhi::UniquePipeline> tonemap_;
    std::unordered_map<VkFormat, rhi::UniquePipeline> overlay_;
};

} // namespace lumora::render
