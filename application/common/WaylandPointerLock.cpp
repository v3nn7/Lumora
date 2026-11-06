#include "common/WaylandPointerLock.hpp"

#include "core/Log.hpp"

#if defined(LUMORA_HAS_WAYLAND_POINTER_LOCK)
#include <QGuiApplication>

#include <pointer-constraints-client.h>
#include <relative-pointer-client.h>
#include <wayland-client.h>

#include <cstring>
#endif

namespace lumora::app {

#if defined(LUMORA_HAS_WAYLAND_POINTER_LOCK)

struct WaylandPointerLock::State {
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;
    wl_registry* registry = nullptr;
    zwp_pointer_constraints_v1* constraints = nullptr;
    zwp_relative_pointer_manager_v1* relativeManager = nullptr;
    wl_pointer* pointer = nullptr;  // our own wl_pointer: tracks which surface has the pointer
    zwp_relative_pointer_v1* relative = nullptr;
    zwp_locked_pointer_v1* locked = nullptr;
    wl_surface* focus = nullptr;
    bool active = false;
    double dx = 0.0;
    double dy = 0.0;

    void dispatch() { wl_display_dispatch_queue_pending(display, queue); }

    ~State() {
        if (locked != nullptr) zwp_locked_pointer_v1_destroy(locked);
        if (relative != nullptr) zwp_relative_pointer_v1_destroy(relative);
        if (pointer != nullptr) {
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(pointer)) >= WL_POINTER_RELEASE_SINCE_VERSION) {
                wl_pointer_release(pointer);
            } else {
                wl_pointer_destroy(pointer);
            }
        }
        if (relativeManager != nullptr) zwp_relative_pointer_manager_v1_destroy(relativeManager);
        if (constraints != nullptr) zwp_pointer_constraints_v1_destroy(constraints);
        if (registry != nullptr) wl_registry_destroy(registry);
        if (display != nullptr) wl_display_flush(display);
        if (queue != nullptr) wl_event_queue_destroy(queue);
    }
};

namespace {

using State = WaylandPointerLock::State;

void onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t) {
    auto* s = static_cast<State*>(data);
    if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0 && s->constraints == nullptr) {
        s->constraints = static_cast<zwp_pointer_constraints_v1*>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0 &&
               s->relativeManager == nullptr) {
        s->relativeManager = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    }
}
void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener{onGlobal, onGlobalRemove};

void onEnter(void* data, wl_pointer*, std::uint32_t, wl_surface* surface, wl_fixed_t, wl_fixed_t) {
    static_cast<State*>(data)->focus = surface;
}
void onLeave(void* data, wl_pointer*, std::uint32_t, wl_surface* surface) {
    auto* s = static_cast<State*>(data);
    if (surface == nullptr || surface == s->focus) s->focus = nullptr;  // null: surface already destroyed
}
void onMotion(void*, wl_pointer*, std::uint32_t, wl_fixed_t, wl_fixed_t) {}
void onButton(void*, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void onAxis(void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {}
void onFrame(void*, wl_pointer*) {}
void onAxisSource(void*, wl_pointer*, std::uint32_t) {}
void onAxisStop(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
void onAxisDiscrete(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
void onAxisValue120(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
void onAxisRelativeDirection(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
// Every event up to wl_pointer version 9 has a handler; create() refuses newer seats.
const wl_pointer_listener kPointerListener{onEnter,      onLeave,    onMotion,       onButton,
                                           onAxis,       onFrame,    onAxisSource,   onAxisStop,
                                           onAxisDiscrete, onAxisValue120, onAxisRelativeDirection};
constexpr std::uint32_t kMaxPointerVersion = WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION;

void onRelativeMotion(void* data, zwp_relative_pointer_v1*, std::uint32_t, std::uint32_t, wl_fixed_t dx,
                      wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
    auto* s = static_cast<State*>(data);
    if (!s->active) return;  // unlocked motion reaches Qt as ordinary pointer motion
    s->dx += wl_fixed_to_double(dx);
    s->dy += wl_fixed_to_double(dy);
}
const zwp_relative_pointer_v1_listener kRelativeListener{onRelativeMotion};

void onLocked(void* data, zwp_locked_pointer_v1*) {
    static_cast<State*>(data)->active = true;
    log::debug("Viewport", "pointer locked by the compositor");
}
void onUnlocked(void* data, zwp_locked_pointer_v1*) { static_cast<State*>(data)->active = false; }
const zwp_locked_pointer_v1_listener kLockedListener{onLocked, onUnlocked};

template <typename T>
T* onQueue(T* proxy, wl_event_queue* queue) {
    auto* wrapper = static_cast<T*>(wl_proxy_create_wrapper(proxy));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
    return wrapper;
}

} // namespace

std::unique_ptr<WaylandPointerLock> WaylandPointerLock::create() {
    if (qEnvironmentVariableIsSet("LUMORA_NO_POINTER_LOCK")) return nullptr;  // escape hatch
    auto* native = qGuiApp != nullptr ? qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
    if (native == nullptr || native->display() == nullptr || native->seat() == nullptr) return nullptr;
    if (native->pointer() == nullptr) {
        log::info("Viewport", "Wayland seat has no pointer - mouse look cannot lock the cursor");
        return nullptr;
    }
    wl_seat* seat = native->seat();
    if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(seat)) > kMaxPointerVersion) {
        log::warn("Viewport", "wl_seat version {} is newer than supported ({}) - pointer lock disabled",
                  wl_proxy_get_version(reinterpret_cast<wl_proxy*>(seat)), kMaxPointerVersion);
        return nullptr;
    }

    auto state = std::make_unique<State>();
    state->display = native->display();
    state->queue = wl_display_create_queue(state->display);
    if (state->queue == nullptr) return nullptr;
    wl_display* display = onQueue(state->display, state->queue);
    state->registry = wl_display_get_registry(display);
    wl_proxy_wrapper_destroy(display);
    wl_registry_add_listener(state->registry, &kRegistryListener, state.get());
    if (wl_display_roundtrip_queue(state->display, state->queue) < 0) {
        log::warn("Viewport", "Wayland roundtrip failed - pointer lock disabled");
        return nullptr;
    }
    if (state->constraints == nullptr || state->relativeManager == nullptr) {
        log::info("Viewport", "compositor lacks pointer-constraints/relative-pointer - mouse look cannot lock the cursor");
        return nullptr;
    }

    wl_seat* seatOnQueue = onQueue(seat, state->queue);
    state->pointer = wl_seat_get_pointer(seatOnQueue);
    wl_proxy_wrapper_destroy(seatOnQueue);
    wl_pointer_add_listener(state->pointer, &kPointerListener, state.get());
    state->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(state->relativeManager, state->pointer);
    zwp_relative_pointer_v1_add_listener(state->relative, &kRelativeListener, state.get());
    wl_display_flush(state->display);
    log::debug("Viewport", "Wayland pointer lock available");
    return std::unique_ptr<WaylandPointerLock>(new WaylandPointerLock(std::move(state)));
}

WaylandPointerLock::WaylandPointerLock(std::unique_ptr<State> state) : state_(std::move(state)) {}
WaylandPointerLock::~WaylandPointerLock() = default;

bool WaylandPointerLock::lock() {
    State& s = *state_;
    s.dispatch();  // the enter event for the surface just clicked
    if (s.locked != nullptr) return true;
    if (s.focus == nullptr) {
        log::debug("Viewport", "pointer lock not requested: no surface has the pointer");
        return false;
    }
    // A surface may carry only one constraint; ours is destroyed before another is requested.
    s.locked = zwp_pointer_constraints_v1_lock_pointer(s.constraints, s.focus, s.pointer, nullptr,
                                                       ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    zwp_locked_pointer_v1_add_listener(s.locked, &kLockedListener, &s);
    s.dx = s.dy = 0.0;
    wl_display_flush(s.display);
    log::debug("Viewport", "pointer lock requested");
    return true;
}

void WaylandPointerLock::unlock() {
    State& s = *state_;
    if (s.locked == nullptr) return;
    zwp_locked_pointer_v1_destroy(s.locked);  // the cursor reappears where the lock began
    s.locked = nullptr;
    s.active = false;
    s.dx = s.dy = 0.0;
    wl_display_flush(s.display);
}

bool WaylandPointerLock::isActive() const noexcept { return state_->active; }

glm::vec2 WaylandPointerLock::takeMotion() {
    State& s = *state_;
    s.dispatch();
    const glm::vec2 motion(static_cast<float>(s.dx), static_cast<float>(s.dy));
    s.dx = s.dy = 0.0;
    return motion;
}

#else  // built without Wayland protocol support

struct WaylandPointerLock::State {};
std::unique_ptr<WaylandPointerLock> WaylandPointerLock::create() { return nullptr; }
WaylandPointerLock::WaylandPointerLock(std::unique_ptr<State> state) : state_(std::move(state)) {}
WaylandPointerLock::~WaylandPointerLock() = default;
bool WaylandPointerLock::lock() { return false; }
void WaylandPointerLock::unlock() {}
bool WaylandPointerLock::isActive() const noexcept { return false; }
glm::vec2 WaylandPointerLock::takeMotion() { return glm::vec2(0.0f); }

#endif

} // namespace lumora::app
