#include "common/VulkanViewport.hpp"

#include "common/AppContext.hpp"
#include "core/Log.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Scene.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QTimer>
#include <QVulkanInstance>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <optional>

namespace lumora::app {

namespace {

std::optional<Key> mapKey(int qtKey) {
    switch (qtKey) {
        case Qt::Key_W: return Key::W;
        case Qt::Key_A: return Key::A;
        case Qt::Key_S: return Key::S;
        case Qt::Key_D: return Key::D;
        case Qt::Key_Q: return Key::Q;
        case Qt::Key_E: return Key::E;
        case Qt::Key_F: return Key::F;
        case Qt::Key_Up: return Key::Up;
        case Qt::Key_Down: return Key::Down;
        case Qt::Key_Left: return Key::Left;
        case Qt::Key_Right: return Key::Right;
        case Qt::Key_Space: return Key::Space;
        case Qt::Key_Shift: return Key::LeftShift;
        case Qt::Key_Control: return Key::LeftCtrl;
        case Qt::Key_Alt: return Key::LeftAlt;
        case Qt::Key_Escape: return Key::Escape;
        default: return std::nullopt;
    }
}

/// Keys that drive the camera and must not leak into widgets (tree type-ahead etc.).
bool isFlightKey(int key) {
    switch (key) {
        case Qt::Key_W: case Qt::Key_A: case Qt::Key_S: case Qt::Key_D: case Qt::Key_Q: case Qt::Key_E:
        case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Space:
            return true;
        default:
            return false;
    }
}

/// True while the user is typing somewhere, so keys belong to that widget.
bool textInputHasFocus() {
    QWidget* w = QApplication::focusWidget();
    if (w == nullptr) return false;
    if (qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QPlainTextEdit*>(w) ||
        qobject_cast<QTextEdit*>(w)) {
        return true;
    }
    auto* combo = qobject_cast<QComboBox*>(w);
    return combo != nullptr && combo->isEditable();
}

std::optional<MouseButton> mapButton(Qt::MouseButton b) {
    switch (b) {
        case Qt::LeftButton: return MouseButton::Left;
        case Qt::RightButton: return MouseButton::Right;
        case Qt::MiddleButton: return MouseButton::Middle;
        default: return std::nullopt;
    }
}

} // namespace

VulkanViewport::VulkanViewport(AppContext& app, ViewportClient& client)
    : app_(app), client_(client), emptyScene_(std::make_unique<Scene>("Empty")) {
    setSurfaceType(QSurface::VulkanSurface);
    if (QVulkanInstance* inst = app_.vulkanInstance()) {
        setVulkanInstance(inst);
    } else {
        failed_ = true;
    }
    // Wayland does not allow clients to warp the pointer; elsewhere we recenter while looking.
    canWarpCursor_ = !QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    vsync_ = app_.options().vsync.value_or(true);
    if (QCoreApplication::instance() != nullptr) QCoreApplication::instance()->installEventFilter(this);
    // Hiding the window (e.g. its container, when the editor closes) makes Qt's Wayland backend
    // destroy the VkSurface right away, before any hide/expose/SurfaceAboutToBeDestroyed event
    // reaches us. visibleChanged is emitted first, so the swapchain is released in time.
    connect(this, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) releaseTarget();
    });
    frameTimer_.start();
    paceTimer_.start();
}

VulkanViewport::~VulkanViewport() { releaseTarget(); }

rhi::PresentPolicy VulkanViewport::policy() const noexcept {
    return vsync_ ? rhi::PresentPolicy::Vsync : rhi::PresentPolicy::Uncapped;
}

void VulkanViewport::setVsync(bool vsync) {
    vsync_ = vsync;
    if (target_) target_->setPolicy(policy());
}

void VulkanViewport::scheduleNextFrame() {
    if (updatePending_ || failed_) return;
    updatePending_ = true;
    // We drive frames with our own precise timer instead of QWindow::requestUpdate(): on
    // Wayland Qt ties update requests to frame callbacks of surfaces *it* commits, and a Vulkan
    // surface is committed by the driver, so requests stall after the first frame.
    // Presentation never blocks this (UI) thread, so with vsync we pace to the screen refresh
    // rate ourselves; FIFO presents already throttle and uncapped mode runs flat out.
    double waitMs = 0.0;
    if (vsync_ && target_ && !target_->swapchain().presentBlocks()) {
        const double hz = screen() != nullptr && screen()->refreshRate() > 1.0 ? screen()->refreshRate() : 60.0;
        waitMs = std::max(0.0, 1000.0 / hz - static_cast<double>(paceTimer_.nsecsElapsed()) * 1e-6);
    }
    QTimer::singleShot(static_cast<int>(waitMs), Qt::PreciseTimer, this, [this] {
        updatePending_ = false;
        renderFrame();
    });
}

VkExtent2D VulkanViewport::pixelExtent() const {
    const qreal dpr = devicePixelRatio();
    return {static_cast<std::uint32_t>(std::max(1.0, std::round(width() * dpr))),
            static_cast<std::uint32_t>(std::max(1.0, std::round(height() * dpr)))};
}

void VulkanViewport::fail(const QString& message) {
    if (failed_) return;
    failed_ = true;
    log::error("Viewport", "{}", message.toStdString());
    emit renderFailed(message);
}

bool VulkanViewport::ensureTarget() {
    if (target_) return true;
    render::Renderer* renderer = app_.renderer();
    if (renderer == nullptr) {
        fail(app_.rendererError());
        return false;
    }
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
    if (surface == VK_NULL_HANDLE) {
        return false;  // platform window not ready yet; retried on the next expose
    }
    try {
        target_ = std::make_unique<render::PresentTarget>(renderer->context(), surface, pixelExtent(), policy());
    } catch (const std::exception& e) {
        fail(QStringLiteral("Cannot create swapchain: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    return true;
}

void VulkanViewport::releaseTarget() {
    if (!target_) return;
    if (render::Renderer* renderer = app_.renderer()) {
        try {
            renderer->waitIdle();
        } catch (const std::exception& e) {
            log::error("Viewport", "waitIdle failed while releasing swapchain: {}", e.what());
        }
    }
    target_.reset();
}

bool VulkanViewport::event(QEvent* e) {
    switch (e->type()) {
        case QEvent::Enter:
            updateEngagement(true, clickFocus_);  // hovering is enough to fly with the keyboard
            break;
        case QEvent::Leave:
            updateEngagement(false, clickFocus_);
            break;
        case QEvent::UpdateRequest:
            scheduleNextFrame();
            return true;
        case QEvent::PlatformSurface:
            if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                releaseTarget();
            }
            break;
        default:
            break;
    }
    return QWindow::event(e);
}

void VulkanViewport::exposeEvent(QExposeEvent*) {
    log::debug("Viewport", "expose: {}", isExposed() ? "visible" : "hidden - rendering paused");
    if (isExposed()) {
        scheduleNextFrame();
    } else {
        // Like QVulkanWindow: drop the swapchain while unexposed. Qt's Wayland backend destroys
        // the VkSurface when a window is hidden (before SurfaceAboutToBeDestroyed), and a
        // swapchain outliving its surface crashes the driver on destruction.
        releaseTarget();
    }

void VulkanViewport::renderFrame() {
    if (failed_ || !isExposed()) return;  // exposeEvent restarts the loop
    if (!ensureTarget()) {
        if (!failed_) scheduleNextFrame();
        return;
    }
    paceTimer_.restart();
    const double dt = std::clamp(static_cast<double>(frameTimer_.nsecsElapsed()) * 1e-9, 0.0, 0.25);
    frameIntervalMs_ = dt * 1000.0;
    frameTimer_.restart();

    client_.viewportTick(static_cast<float>(dt), input_, isActive());
    input_.endFrame();

    const Scene* scene = client_.viewportScene();
    if (scene == nullptr) scene = emptyScene_.get();
    const VkExtent2D extent = pixelExtent();
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    target_->resize(extent);
    try {
        const auto result = app_.renderer()->render(*target_, *scene, client_.viewportCamera(aspect),
                                                    client_.viewportSettings());
        if (result == render::FrameResult::SurfaceLost) {
            log::warn("Viewport", "surface lost - recreating");
            releaseTarget();
        }
        if (!diagTimer_.isValid()) diagTimer_.start();
        (result == render::FrameResult::Rendered ? diagRendered_ : diagSkipped_)++;
        if (diagTimer_.elapsed() >= 1000) {
            log::debug("Viewport", "{:.1f} frames/s presented, {} skipped (no swapchain image ready) in {:.1f} s",
                       diagRendered_ * 1000.0 / static_cast<double>(diagTimer_.elapsed()), diagSkipped_,
                       static_cast<double>(diagTimer_.elapsed()) / 1000.0);
            diagRendered_ = diagSkipped_ = 0;
            diagTimer_.restart();
        }
        if (result != render::FrameResult::Rendered) {
            scheduleNextFrame();
            return;
        }
    } catch (const rhi::DeviceLostError& e) {
        fail(QStringLiteral("The GPU device was lost (driver reset or hang). Rendering is stopped; save your "
                            "work and restart Lumora.\n\n%1")
                 .arg(QString::fromUtf8(e.what())));
        return;
    } catch (const std::exception& e) {
        fail(QStringLiteral("Rendering failed: %1").arg(QString::fromUtf8(e.what())));
        return;
    }
    emit frameRendered();
    scheduleNextFrame();
}

QImage VulkanViewport::captureFrame(QSize pixels) {
    render::Renderer* renderer = app_.renderer();
    if (renderer == nullptr || failed_ || pixels.isEmpty()) return {};
    const Scene* scene = client_.viewportScene();
    if (scene == nullptr) scene = emptyScene_.get();
    const float aspect = static_cast<float>(pixels.width()) / static_cast<float>(pixels.height());
    try {
        render::RenderSettings settings = client_.viewportSettings();
        settings.blockOnAssets = true;  // a snapshot shows the finished frame
        auto image = renderer->capture(*scene, client_.viewportCamera(aspect),
                                       {static_cast<std::uint32_t>(pixels.width()), static_cast<std::uint32_t>(pixels.height())},
                                       settings);
        if (!image) return {};
        return QImage(image->rgba.data(), static_cast<int>(image->width), static_cast<int>(image->height),
                      QImage::Format_RGBA8888)
            .copy();
    } catch (const std::exception& e) {
        log::error("Viewport", "capture failed: {}", e.what());
        return {};
    }
}

QWindow* VulkanViewport::topLevelWindow() {
    QWindow* w = this;
    while (w->parent() != nullptr) w = w->parent();
    return w;
}

void VulkanViewport::updateEngagement(bool hovered, bool clickFocus) {
    const bool was = engaged();
    hovered_ = hovered;
    clickFocus_ = clickFocus;
    if (was && !engaged()) input_.releaseAll();  // never leave a key "stuck" down
}

bool VulkanViewport::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseButtonPress:
            // A click in another window (panel, menu, dialog) hands the keyboard back to it.
            if (watched != this && qobject_cast<QWindow*>(watched) != nullptr) updateEngagement(hovered_, false);
            break;
        case QEvent::ApplicationDeactivate:
            input_.releaseAll();
            break;
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            // Keys delivered to this window go through keyPressEvent. Only take keys that reached
            // our own top-level window (seen once, before any widget), never other windows'.
            QWindow* top = topLevelWindow();
            if (top == this || watched != top) break;
            if (!engaged() || textInputHasFocus()) break;
            auto* key = static_cast<QKeyEvent*>(event);
            const bool down = event->type() == QEvent::KeyPress;
            if (!key->isAutoRepeat()) {
                if (auto k = mapKey(key->key())) input_.setKey(*k, down);
            }
            if (isFlightKey(key->key())) return true;  // the camera owns it
            if (down && client_.viewportKeyPressed(key->key(), key->modifiers())) return true;
            break;
        }
        default:
            break;
    }
    return QWindow::eventFilter(watched, event);
}

void VulkanViewport::keyPressEvent(QKeyEvent* e) {
    if (!e->isAutoRepeat()) {
        if (auto k = mapKey(e->key())) input_.setKey(*k, true);
    }
    if (!client_.viewportKeyPressed(e->key(), e->modifiers())) {
        QWindow::keyPressEvent(e);
    }
}

void VulkanViewport::keyReleaseEvent(QKeyEvent* e) {
    if (!e->isAutoRepeat()) {
        if (auto k = mapKey(e->key())) input_.setKey(*k, false);
    }
}

glm::vec2 VulkanViewport::toNdc(QPointF position) const {
    return {static_cast<float>(position.x() / std::max(width(), 1)) * 2.0f - 1.0f,
            static_cast<float>(position.y() / std::max(height(), 1)) * 2.0f - 1.0f};
}

void VulkanViewport::mousePressEvent(QMouseEvent* e) {
    requestActivate();
    updateEngagement(hovered_, true);
    emit pointerPressed();
    lastMouse_ = e->position();
    if (e->button() == Qt::LeftButton) {
        pressPos_ = e->position();
        leftDragged_ = false;
        pressConsumed_ = client_.viewportMousePressed(toNdc(e->position()), e->button(), e->modifiers());
        if (pressConsumed_) return;  // e.g. gizmo drag: no camera input, no click
    }
    if (auto b = mapButton(e->button())) input_.setMouseButton(*b, true);
    if (e->button() == Qt::RightButton) {
        looking_ = true;
        setCursor(Qt::BlankCursor);
    }
}

void VulkanViewport::mouseReleaseEvent(QMouseEvent* e) {
    if (auto b = mapButton(e->button())) input_.setMouseButton(*b, false);
    client_.viewportMouseReleased(toNdc(e->position()), e->button());
    if (e->button() == Qt::LeftButton && pressConsumed_) {
        pressConsumed_ = false;
    } else if (e->button() == Qt::LeftButton && !leftDragged_ && width() > 0 && height() > 0) {
        client_.viewportClicked(toNdc(e->position()));
    } else if (e->button() == Qt::RightButton) {
        looking_ = false;
        unsetCursor();
    }
}

void VulkanViewport::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    client_.viewportMouseMoved(toNdc(pos), e->modifiers());
    const QPointF delta = pos - lastMouse_;
    input_.addMouseDelta({static_cast<float>(delta.x()), static_cast<float>(delta.y())});
    input_.setMousePosition({static_cast<float>(pos.x()), static_cast<float>(pos.y())});
    lastMouse_ = pos;
    if ((e->buttons() & Qt::LeftButton) && (pos - pressPos_).manhattanLength() > 4.0) {
        leftDragged_ = true;
    }
    if (looking_ && canWarpCursor_) {
        const QPoint center(width() / 2, height() / 2);
        if ((pos - QPointF(center)).manhattanLength() > 50.0) {
            QCursor::setPos(mapToGlobal(center));
            lastMouse_ = QPointF(center);
        }
    }
}

void VulkanViewport::wheelEvent(QWheelEvent* e) {
    input_.addScroll(static_cast<float>(e->angleDelta().y()) / 120.0f);
}

void VulkanViewport::focusOutEvent(QFocusEvent*) {
    input_.releaseAll();
    if (looking_) {
        looking_ = false;
        unsetCursor();
    }
}

} // namespace lumora::app
