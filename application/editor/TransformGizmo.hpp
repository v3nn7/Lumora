#pragma once

#include "renderer/Overlay.hpp"
#include "scene/Camera.hpp"
#include "scene/Components.hpp"

#include <entt/entity/entity.hpp>
#include <glm/vec2.hpp>

#include <optional>

namespace lumora {
class Scene;
}

namespace lumora::editor {

enum class GizmoMode { Translate, Rotate, Scale };

/// Translate / rotate / scale manipulator. Handles are picked in screen space (so thin handles
/// stay easy to grab), drags are constrained to the picked axis: translation along world axes,
/// rotation about world axes, scaling along the entity's local axes. Ctrl snaps (0.25 m, 15°,
/// 0.1x). Transforms of parented entities are converted back into parent space.
class TransformGizmo {
public:
    [[nodiscard]] GizmoMode mode() const noexcept { return mode_; }
    void setMode(GizmoMode mode) noexcept;

    void build(const Scene& scene, entt::entity target, const CameraView& view, render::OverlayGeometry& out) const;
    /// Updates the hovered handle (highlight). Returns true if it changed.
    bool hover(const Scene& scene, entt::entity target, const CameraView& view, glm::vec2 ndc);
    /// Starts a drag if a handle is under the cursor.
    bool begin(const Scene& scene, entt::entity target, const CameraView& view, glm::vec2 ndc);
    void drag(Scene& scene, entt::entity target, const CameraView& view, glm::vec2 ndc, bool snap);
    void end() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_ != Handle::None; }
    [[nodiscard]] const TransformComponent& startTransform() const noexcept { return start_; }

    /// Screen-relative gizmo size (fraction of the half view height).
    static constexpr float kScreenSize = 0.3f;

private:
    enum class Handle { None, X, Y, Z, Center };
    struct Frame {
        glm::vec3 origin{0.0f};
        glm::vec3 axes[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        float size = 1.0f;
    };

    [[nodiscard]] Frame frameFor(const Scene& scene, entt::entity target, const CameraView& view) const;
    [[nodiscard]] Handle pick(const Frame& frame, const CameraView& view, glm::vec2 ndc) const;
    [[nodiscard]] static std::optional<float> axisParameter(const glm::vec3& origin, const glm::vec3& axis,
                                                            const CameraView& view, glm::vec2 ndc);

    GizmoMode mode_ = GizmoMode::Translate;
    Handle hovered_ = Handle::None;
    Handle active_ = Handle::None;

    // Drag state (captured at begin()).
    Frame dragFrame_;
    TransformComponent start_;
    glm::mat4 parentWorld_{1.0f};
    float startParameter_ = 0.0f;
    glm::vec3 startVector_{0.0f};
    bool startVectorValid_ = false;
    glm::vec2 startNdc_{0.0f};
};

} // namespace lumora::editor
