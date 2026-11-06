#pragma once

#include "common/ModuleExport.hpp"
#include "renderer/Sprites.hpp"

#include <entt/entity/entity.hpp>

#include <vector>

namespace lumora {
class Scene;
}

namespace lumora::editor {

/// The editor's floating icons for things without a visible shape: lights (sun, bulb, spot,
/// tinted with the light's colour), cameras, sound sources and particle emitters. Each is a
/// round badge with a picture (SVGs in common/icons/scene) that always faces the viewer and
/// keeps its size on screen; clicking one selects its entity. Editor only: games never see them.
class LUMORA_EDITOR_API SceneIcons {
public:
    explicit SceneIcons(float size = 40.0f) : size_(size) {}

    /// Fills `out` with the scene's icons as seen from `eye` (they fade out right next to it);
    /// rebuilds the atlas when the pixel ratio changes.
    void build(const Scene& scene, const glm::vec3& eye, const std::vector<entt::entity>& selected, float pixelRatio,
               render::SpriteBatch& out);
    /// The entity whose icon covers a target pixel (target = window pixels x pixel ratio).
    [[nodiscard]] static entt::entity pick(const render::SpriteBatch& icons, const CameraView& camera,
                                           glm::vec2 targetPixels, glm::vec2 pixel);
    [[nodiscard]] const render::SpriteAtlas& atlas() const noexcept { return atlas_; }
    [[nodiscard]] float size() const noexcept { return size_; }

    enum class Picture { Badge, BadgeSelected, Sun, Bulb, Spot, Camera, Speaker, Sparkles, Count };

private:
    void rasterize(float pixelRatio);

    float size_;
    float atlasRatio_ = 0.0f;
    render::SpriteAtlas atlas_;
    glm::vec4 uv_[static_cast<int>(Picture::Count)]{};
};

} // namespace lumora::editor
