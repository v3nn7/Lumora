#pragma once

#include <glm/vec2.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace lumora {

enum class Key : std::uint8_t {
    W, A, S, D, Q, E, F,
    Up, Down, Left, Right,
    Space, LeftShift, LeftCtrl, LeftAlt, Escape,
    Count
};

enum class MouseButton : std::uint8_t { Left, Right, Middle, Count };

/// Platform-agnostic input snapshot. The windowing layer feeds events; consumers read it
/// once per frame and call endFrame() to reset per-frame deltas.
class InputState {
public:
    void setKey(Key key, bool down) noexcept;
    void setMouseButton(MouseButton button, bool down) noexcept;
    void setMousePosition(glm::vec2 position) noexcept;
    void addMouseDelta(glm::vec2 delta) noexcept;
    void addScroll(float steps) noexcept;
    void releaseAll() noexcept;
    void endFrame() noexcept;

    [[nodiscard]] bool isDown(Key key) const noexcept { return keys_[index(key)]; }
    [[nodiscard]] bool isDown(MouseButton button) const noexcept { return buttons_[index(button)]; }
    [[nodiscard]] bool wasPressed(Key key) const noexcept { return keys_[index(key)] && !prevKeys_[index(key)]; }
    [[nodiscard]] glm::vec2 mousePosition() const noexcept { return mousePosition_; }
    [[nodiscard]] glm::vec2 mouseDelta() const noexcept { return mouseDelta_; }
    [[nodiscard]] float scrollDelta() const noexcept { return scroll_; }

private:
    template <class E>
    static constexpr std::size_t index(E e) noexcept { return static_cast<std::size_t>(e); }

    std::array<bool, static_cast<std::size_t>(Key::Count)> keys_{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> prevKeys_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttons_{};
    glm::vec2 mousePosition_{0.0f};
    glm::vec2 mouseDelta_{0.0f};
    float scroll_ = 0.0f;
};

} // namespace lumora
