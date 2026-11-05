#pragma once

#include "input/InputState.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/vulkan/Swapchain.hpp"
#include "scene/Camera.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QWindow>

#include <memory>

namespace lumora {
class Scene;
}

namespace lumora::render {
class PresentTarget;
}

namespace lumora::app {

class AppContext;

/// Supplies content to a VulkanViewport; implemented by the editor and the player.
class ViewportClient {
public:
    virtual ~ViewportClient() = default;
    /// Called once per frame before rendering, with the frame delta and current input.
    virtual void viewportTick(float dt, const InputState& input, bool hasFocus) = 0;
    [[nodiscard]] virtual const Scene* viewportScene() const = 0;
    [[nodiscard]] virtual CameraView viewportCamera(float aspect) const = 0;
    [[nodiscard]] virtual render::RenderSettings viewportSettings() const = 0;
    /// Left click without drag; ndc in [-1,1], +Y down.
    virtual void viewportClicked(glm::vec2 ndc) { (void)ndc; }
    /// Raw pointer events (ndc as above). Returning true from a press consumes it: no click and
    /// no camera interaction follow (used by gizmo drags).
    virtual bool viewportMousePressed(glm::vec2 ndc, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
        (void)ndc;
        (void)button;
        (void)modifiers;
        return false;
    }
    virtual void viewportMouseMoved(glm::vec2 ndc, Qt::KeyboardModifiers modifiers) {
        (void)ndc;
        (void)modifiers;
    }
    virtual void viewportMouseReleased(glm::vec2 ndc, Qt::MouseButton button) {
        (void)ndc;
        (void)button;
    }
    /// Key presses that reach the viewport (Qt key code + modifiers). Return true if handled.
    virtual bool viewportKeyPressed(int key, Qt::KeyboardModifiers modifiers) {
        (void)key;
        (void)modifiers;
        return false;
    }
};

/// Native Vulkan surface (QWindow) driven by the shared Renderer. Embed into widget UIs with
/// QWidget::createWindowContainer. Renders continuously while exposed; owns its swapchain and
/// releases it before Qt destroys the underlying platform surface.
class VulkanViewport final : public QWindow {
    Q_OBJECT
public:
    VulkanViewport(AppContext& app, ViewportClient& client);
    ~VulkanViewport() override;

    void setVsync(bool vsync);
    [[nodiscard]] double frameIntervalMs() const noexcept { return frameIntervalMs_; }
    [[nodiscard]] bool hasFailed() const noexcept { return failed_; }
    [[nodiscard]] const InputState& input() const noexcept { return input_; }
    /// Offscreen render of what the viewport currently shows (null image if unavailable).
    [[nodiscard]] QImage captureFrame(QSize pixels);

signals:
    void frameRendered();
    void renderFailed(const QString& message);
    /// A mouse button went down in the viewport; embedders move widget focus to its container
    /// (on Wayland the embedded window itself never receives keyboard focus).
    void pointerPressed();

protected:
    bool event(QEvent* e) override;
    void exposeEvent(QExposeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    /// Keyboard routing for an embedded viewport: on Wayland the embedded (subsurface) window
    /// never gets keyboard focus, so keys reach the top-level window and are taken from there.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void renderFrame();
    void scheduleNextFrame();
    [[nodiscard]] rhi::PresentPolicy policy() const noexcept;
    bool ensureTarget();
    void releaseTarget();
    void fail(const QString& message);
    [[nodiscard]] VkExtent2D pixelExtent() const;
    [[nodiscard]] glm::vec2 toNdc(QPointF position) const;
    /// Keyboard drives the viewport while it is hovered, was clicked last, or is looking.
    [[nodiscard]] bool engaged() const noexcept { return hovered_ || clickFocus_ || looking_; }
    void updateEngagement(bool hovered, bool clickFocus);
    [[nodiscard]] QWindow* topLevelWindow();

    AppContext& app_;
    ViewportClient& client_;
    std::unique_ptr<render::PresentTarget> target_;
    std::unique_ptr<Scene> emptyScene_;
    InputState input_;
    QElapsedTimer frameTimer_;
    QElapsedTimer paceTimer_;
    QPointF lastMouse_;
    QPointF pressPos_;
    bool leftDragged_ = false;
    bool hovered_ = false;     // pointer over the viewport
    bool clickFocus_ = false;  // last click landed in the viewport
    bool pressConsumed_ = false;
    bool looking_ = false;
    bool canWarpCursor_ = false;
    bool failed_ = false;
    bool vsync_ = true;
    bool updatePending_ = false;
    double frameIntervalMs_ = 0.0;
    QElapsedTimer diagTimer_;
    int diagRendered_ = 0;
    int diagSkipped_ = 0;
};

} // namespace lumora::app
