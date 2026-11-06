#pragma once

#include "common/VulkanViewport.hpp"
#include "editor/SceneIcons.hpp"
#include "editor/TransformGizmo.hpp"
#include "renderer/Overlay.hpp"
#include "scene/Camera.hpp"
#include "scene/Picking.hpp"

#include <functional>

namespace lumora {
struct HudCanvas;
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
    std::function<bool(const QList<QUrl>&)> onAcceptsDrop;            // files dragged over the viewport
    std::function<void(const QList<QUrl>&, glm::vec2)> onDrop;        // files dropped (ndc)

    [[nodiscard]] FlyCameraController& camera() noexcept { return camera_; }
    [[nodiscard]] TransformGizmo& gizmo() noexcept { return gizmo_; }
    void frameEntity(entt::entity e);
    void frameSelection();

    /// Collaborators: where their camera is and what they have selected (drawn in their colour).
    struct RemoteMarker {
        glm::vec3 position{0.0f};
        glm::vec3 forward{0.0f, 0.0f, -1.0f};
        glm::vec4 color{1.0f};
        bool hasPose = false;
        Uuid selection;
    };
    std::vector<RemoteMarker> remoteMarkers;

    bool showGrid = true;
    bool showIcons = true;              // floating light / camera / sound icons while editing
    glm::vec2 viewportPixels{0.0f};     // the viewport's size in target pixels (set by the window)
    float pixelRatio = 1.0f;            // target pixels per window pixel
    AssetLibrary* assets = nullptr;  // project asset library (meshes for rendering and picking)
    const HudCanvas* hud = nullptr;  // the game's HUD while playing
    const DebugDraw* debug = nullptr;  // behaviours' debug shapes (play mode)
    bool pilotSceneCamera = true;  // in play mode, look through the scene's primary camera

    void viewportTick(float dt, const InputState& input, bool hasFocus) override;
    [[nodiscard]] const Scene* viewportScene() const override;
    [[nodiscard]] CameraView viewportCamera(float aspect) const override;
    [[nodiscard]] render::RenderSettings viewportSettings() const override;
    void viewportClicked(glm::vec2 ndc) override;
    bool viewportAcceptsDrop(const QList<QUrl>& urls) override { return onAcceptsDrop && onAcceptsDrop(urls); }
    void viewportDropped(const QList<QUrl>& urls, glm::vec2 ndc) override {
        if (onDrop) onDrop(urls, ndc);
    }
    /// The world ray under a viewport position (ndc) for the current camera.
    [[nodiscard]] Ray rayAt(glm::vec2 ndc) const;
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
    void drawRemoteMarkers(const Scene& scene);
    /// Range sphere, cone or direction of the selected light (editing only).
    void drawLightOf(const Scene& scene, entt::entity e);

    EditorContext& ctx_;
    FlyCameraController camera_;
    SceneIcons icons_;
    render::SpriteBatch sprites_;
    mutable float lastAspect_ = 16.0f / 9.0f;
    struct GroupMember {
        Uuid id;
        TransformComponent transform;  // local, at the start of the drag
        glm::mat4 world;
    };
    void moveGroup(Scene& scene);
    std::vector<GroupMember> groupStart_;
    glm::mat4 primaryWorldStart_{1.0f};
    Qt::KeyboardModifiers pressModifiers_;
    TransformGizmo gizmo_;
    render::OverlayGeometry overlay_;
    Uuid dragTarget_;
};

} // namespace lumora::editor
