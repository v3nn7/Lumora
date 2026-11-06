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
    /// Presses and releases so far (never reset): a consumer that runs at its own rate (the
    /// fixed-step simulation) compares counts to see every click, even one shorter than a frame.
    [[nodiscard]] std::uint32_t pressCount(MouseButton b) const noexcept { return buttonPresses_[index(b)]; }
    [[nodiscard]] std::uint32_t releaseCount(MouseButton b) const noexcept { return buttonReleases_[index(b)]; }
    [[nodiscard]] std::uint32_t pressCount(Key key) const noexcept { return keyPresses_[index(key)]; }

private:
    template <class E>
    static constexpr std::size_t index(E e) noexcept { return static_cast<std::size_t>(e); }

    std::array<bool, static_cast<std::size_t>(Key::Count)> keys_{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> prevKeys_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttons_{};
    std::array<std::uint32_t, static_cast<std::size_t>(MouseButton::Count)> buttonPresses_{};
    std::array<std::uint32_t, static_cast<std::size_t>(MouseButton::Count)> buttonReleases_{};
    std::array<std::uint32_t, static_cast<std::size_t>(Key::Count)> keyPresses_{};
    glm::vec2 mousePosition_{0.0f};
    glm::vec2 mouseDelta_{0.0f};
    float scroll_ = 0.0f;
};

} // namespace lumora
