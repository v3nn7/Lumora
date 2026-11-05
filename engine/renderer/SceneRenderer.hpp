#pragma once

#include "renderer/RenderGraph.hpp"
#include "renderer/vulkan/Resources.hpp"
#include "scene/Camera.hpp"

#include <entt/entity/entity.hpp>

#include <unordered_map>

namespace lumora {
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
    entt::entity selected = entt::null;
};

struct SceneDrawStats {
    std::uint32_t drawCalls = 0;
    std::uint32_t triangles = 0;
    std::uint32_t visibleObjects = 0;
    std::uint32_t culledObjects = 0;
    std::uint32_t lights = 0;
};

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
    VkPipeline tonemapPipeline(VkFormat outputFormat);

    RenderContext& ctx_;
    rhi::UniquePipeline sky_;
    rhi::UniquePipeline opaque_;
    rhi::UniquePipeline grid_;
    std::unordered_map<VkFormat, rhi::UniquePipeline> tonemap_;
};

} // namespace lumora::render
