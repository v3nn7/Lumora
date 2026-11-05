#pragma once

#include <glm/vec2.hpp>

#include <memory>

namespace lumora::app {

/// Holds the pointer in place while the viewport camera looks around. Wayland does not let
/// clients warp the cursor, so this uses the pointer-constraints and relative-pointer protocols:
/// the cursor stays where the button was pressed and motion arrives as relative deltas.
///
/// All protocol objects live on a private event queue that is dispatched only from here (on the
/// UI thread), so nothing interferes with Qt's own Wayland objects.
class WaylandPointerLock {
public:
    /// Null when not running on Wayland, when the compositor lacks either protocol, or when
    /// Lumora was built without Wayland support.
    [[nodiscard]] static std::unique_ptr<WaylandPointerLock> create();
    ~WaylandPointerLock();
    WaylandPointerLock(const WaylandPointerLock&) = delete;
    WaylandPointerLock& operator=(const WaylandPointerLock&) = delete;

    /// Requests a lock on the surface under the pointer. False if no surface has the pointer.
    bool lock();
    void unlock();
    /// True once the compositor has activated the lock (Qt then receives no pointer motion).
    [[nodiscard]] bool isActive() const noexcept;
    /// Processes pending protocol events and returns the relative motion (logical pixels,
    /// accelerated like normal cursor motion) received while locked since the last call.
    glm::vec2 takeMotion();

    struct State;

private:
    explicit WaylandPointerLock(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace lumora::app
