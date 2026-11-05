#pragma once

#include "common/VulkanViewport.hpp"
#include "editor/TransformGizmo.hpp"
#include "renderer/Overlay.hpp"
#include "scene/Camera.hpp"

#include <functional>

namespace lumora {
class AssetLibrary;
}

namespace lumora::editor {

class EditorContext;

/// Editor behaviour for the 3D viewport: fly camera, click selection, play-mode camera and
/// forwarding of editor shortcuts that arrive while the native viewport has focus.
class EditorViewportClient final : public app::ViewportClient {
public:
    explicit EditorViewportClient(EditorContext& ctx);

    std::function<void(float)> onSimulate;                          // advances play mode
    std::function<bool(int, Qt::KeyboardModifiers)> onShortcut;      // window-level shortcuts

    [[nodiscard]] FlyCameraController& camera() noexcept { return camera_; }
    [[nodiscard]] TransformGizmo& gizmo() noexcept { return gizmo_; }
    void frameEntity(entt::entity e);
    void frameSelection();

    bool showGrid = true;
    AssetLibrary* assets = nullptr;  // project asset library (meshes for rendering and picking)
    bool pilotSceneCamera = true;  // in play mode, look through the scene's primary camera

    void viewportTick(float dt, const InputState& input, bool hasFocus) override;
    [[nodiscard]] const Scene* viewportScene() const override;
    [[nodiscard]] CameraView viewportCamera(float aspect) const override;
    [[nodiscard]] render::RenderSettings viewportSettings() const override;
    void viewportClicked(glm::vec2 ndc) override;
    bool viewportKeyPressed(int key, Qt::KeyboardModifiers modifiers) override;
    bool viewportMousePressed(glm::vec2 ndc, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) override;
    void viewportMouseMoved(glm::vec2 ndc, Qt::KeyboardModifiers modifiers) override;
    void viewportMouseReleased(glm::vec2 ndc, Qt::MouseButton button) override;

private:
    [[nodiscard]] bool viewingThroughSceneCamera() const;
    /// The gizmo is shown for the selection while editing (not in play mode).
    [[nodiscard]] bool gizmoVisible() const;
    /// Translucent collider outline for the selected entity (editing only).
    void drawColliderOf(const Scene& scene, entt::entity e);

    EditorContext& ctx_;
    FlyCameraController camera_;
    mutable float lastAspect_ = 16.0f / 9.0f;
    TransformGizmo gizmo_;
    render::OverlayGeometry overlay_;
    Uuid dragTarget_;
};

} // namespace lumora::editor
