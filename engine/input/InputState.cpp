#include "input/InputState.hpp"

namespace lumora {

void InputState::setKey(Key key, bool down) noexcept {
    if (key == Key::Count) return;
    if (down && !keys_[index(key)]) ++keyPresses_[index(key)];  // auto-repeat is not a new press
    keys_[index(key)] = down;
}

void InputState::setMouseButton(MouseButton button, bool down) noexcept {
    if (button == MouseButton::Count) return;
    const bool was = buttons_[index(button)];
    if (down && !was) ++buttonPresses_[index(button)];
    if (!down && was) ++buttonReleases_[index(button)];
    buttons_[index(button)] = down;
}

void InputState::setMousePosition(glm::vec2 position) noexcept { mousePosition_ = position; }

void InputState::setGamepadConnected(bool connected) noexcept {
    gamepadConnected_ = connected;
    if (!connected) {
        padButtons_.fill(false);
        padAxes_.fill(0.0f);
    }
}

void InputState::setGamepadButton(GamepadButton button, bool down) noexcept {
    if (button == GamepadButton::Count) return;
    if (down && !padButtons_[index(button)]) ++padPresses_[index(button)];
    padButtons_[index(button)] = down;
}

void InputState::setGamepadAxis(GamepadAxis axis, float value) noexcept {
    if (axis != GamepadAxis::Count) padAxes_[index(axis)] = value;
}

void InputState::addMouseDelta(glm::vec2 delta) noexcept { mouseDelta_ += delta; }

void InputState::addScroll(float steps) noexcept { scroll_ += steps; }

void InputState::releaseAll() noexcept {
    keys_.fill(false);
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (buttons_[i]) ++buttonReleases_[i];
    }
    buttons_.fill(false);
    mouseDelta_ = glm::vec2(0.0f);
    scroll_ = 0.0f;
}

void InputState::endFrame() noexcept {
    prevKeys_ = keys_;
    mouseDelta_ = glm::vec2(0.0f);
    scroll_ = 0.0f;
}

} // namespace lumora
