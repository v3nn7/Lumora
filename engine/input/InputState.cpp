#include "input/InputState.hpp"

namespace lumora {

void InputState::setKey(Key key, bool down) noexcept {
    if (key != Key::Count) keys_[index(key)] = down;
}

void InputState::setMouseButton(MouseButton button, bool down) noexcept {
    if (button != MouseButton::Count) buttons_[index(button)] = down;
}

void InputState::setMousePosition(glm::vec2 position) noexcept { mousePosition_ = position; }

void InputState::addMouseDelta(glm::vec2 delta) noexcept { mouseDelta_ += delta; }

void InputState::addScroll(float steps) noexcept { scroll_ += steps; }

void InputState::releaseAll() noexcept {
    keys_.fill(false);
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
