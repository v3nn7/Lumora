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
    // Appended later (the order is part of the scripting ABI):
    Enter, Tab, Backspace,
    B, C, G, H, I, J, K, L, M, N, O, P, R, T, U, V, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Count
};

enum class MouseButton : std::uint8_t { Left, Right, Middle, Count };

/// Xbox layout (other pads map to the same positions).
enum class GamepadButton : std::uint8_t {
    A, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide, LeftStick, RightStick,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    Count
};
/// Sticks -1..1 (x right, y down, dead zone applied), triggers 0..1.
enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

/// Platform-agnostic input snapshot. The windowing layer feeds events; consumers read it
/// once per frame and call endFrame() to reset per-frame deltas.
class InputState {
public:
    void setKey(Key key, bool down) noexcept;
    void setMouseButton(MouseButton button, bool down) noexcept;
    void setMousePosition(glm::vec2 position) noexcept;
    void addMouseDelta(glm::vec2 delta) noexcept;
    void addScroll(float steps) noexcept;
    void setGamepadConnected(bool connected) noexcept;
    void setGamepadButton(GamepadButton button, bool down) noexcept;
    void setGamepadAxis(GamepadAxis axis, float value) noexcept;
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

    [[nodiscard]] bool gamepadConnected() const noexcept { return gamepadConnected_; }
    [[nodiscard]] bool isDown(GamepadButton b) const noexcept { return padButtons_[index(b)]; }
    [[nodiscard]] float axis(GamepadAxis a) const noexcept { return padAxes_[index(a)]; }
    [[nodiscard]] std::uint32_t pressCount(GamepadButton b) const noexcept { return padPresses_[index(b)]; }

private:
    template <class E>
    static constexpr std::size_t index(E e) noexcept { return static_cast<std::size_t>(e); }

    std::array<bool, static_cast<std::size_t>(Key::Count)> keys_{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> prevKeys_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttons_{};
    std::array<std::uint32_t, static_cast<std::size_t>(MouseButton::Count)> buttonPresses_{};
    std::array<std::uint32_t, static_cast<std::size_t>(MouseButton::Count)> buttonReleases_{};
    std::array<std::uint32_t, static_cast<std::size_t>(Key::Count)> keyPresses_{};
    bool gamepadConnected_ = false;
    std::array<bool, static_cast<std::size_t>(GamepadButton::Count)> padButtons_{};
    std::array<std::uint32_t, static_cast<std::size_t>(GamepadButton::Count)> padPresses_{};
    std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> padAxes_{};
    glm::vec2 mousePosition_{0.0f};
    glm::vec2 mouseDelta_{0.0f};
    float scroll_ = 0.0f;
};

} // namespace lumora
