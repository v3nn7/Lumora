#include "runtime/GameUi.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace lumora {

namespace {

constexpr glm::vec4 kPanel{0.06f, 0.07f, 0.09f, 0.82f};
constexpr glm::vec4 kButton{0.16f, 0.17f, 0.21f, 0.92f};
constexpr glm::vec4 kHover{0.23f, 0.25f, 0.31f, 0.95f};
constexpr glm::vec4 kText{0.95f, 0.96f, 0.98f, 1.0f};
constexpr glm::vec4 kTrack{0.11f, 0.12f, 0.15f, 0.95f};

std::string formatValue(float v) {
    const float a = std::abs(v);
    return a >= 100.0f ? std::format("{:.0f}", v) : a >= 10.0f ? std::format("{:.1f}", v) : std::format("{:.2f}", v);
}

} // namespace

void GameUi::beginStep(const InputState* input, HudCanvas* hud, Navigation navigation) {
    hud_ = hud;
    input_ = input;
    nav_ = navigation;
    lastWidgets_ = widgets_;
    widgets_ = 0;
    if (lastWidgets_ == 0) {
        focus_ = -1;
    } else if (nav_.down) {
        focus_ = (focus_ + 1) % lastWidgets_;
    } else if (nav_.up) {
        focus_ = focus_ <= 0 ? lastWidgets_ - 1 : focus_ - 1;
    } else if (focus_ >= lastWidgets_) {
        focus_ = lastWidgets_ - 1;
    }
    pressed_.fill(false);
    released_.fill(false);
    keyPressed_.fill(false);
    padPressed_.fill(false);
    if (input == nullptr) {
        mouse_ = glm::vec2(-1.0f);
        active_ = 0;
        return;
    }
    const float ratio = hud != nullptr ? hud->pixelRatio : 1.0f;
    mouse_ = input->mousePosition() * ratio;
    for (std::size_t b = 0; b < pressed_.size(); ++b) {
        const auto button = static_cast<MouseButton>(b);
        if (primed_) {
            pressed_[b] = input->pressCount(button) != seenPresses_[b];
            released_[b] = input->releaseCount(button) != seenReleases_[b];
        }
        seenPresses_[b] = input->pressCount(button);
        seenReleases_[b] = input->releaseCount(button);
    }
    for (std::size_t k = 0; k < keyPressed_.size(); ++k) {
        const auto key = static_cast<Key>(k);
        if (primed_) keyPressed_[k] = input->pressCount(key) != seenKeys_[k];
        seenKeys_[k] = input->pressCount(key);
    }
    for (std::size_t b = 0; b < padPressed_.size(); ++b) {
        const auto button = static_cast<GamepadButton>(b);
        if (primed_) padPressed_[b] = input->pressCount(button) != seenPad_[b];
        seenPad_[b] = input->pressCount(button);
    }
    if (primed_ && mouse_ != lastMouse_ && lastMouse_.x >= 0.0f) focus_ = -1;  // the mouse moved: it leads
    lastMouse_ = mouse_;
    primed_ = true;
    // The button is up and was not pressed since: nothing is being held.
    if (!pressed(MouseButton::Left) && !input->isDown(MouseButton::Left)) {
        if (!released(MouseButton::Left)) active_ = 0;
    }
}

void GameUi::beginStep(const InputState* input, HudCanvas* hud) { beginStep(input, hud, Navigation{}); }

bool GameUi::takeFocusSlot() { return widgets_++ == focus_; }

bool GameUi::hovered(glm::vec4 r) const noexcept {
    return mouse_.x >= r.x && mouse_.y >= r.y && mouse_.x < r.x + r.z && mouse_.y < r.y + r.w;
}

std::uint64_t GameUi::idOf(std::string_view label, glm::vec4 rect) noexcept {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a over the label and the position
    for (char c : label) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    h = (h ^ std::bit_cast<std::uint32_t>(std::round(rect.x))) * 1099511628211ull;
    h = (h ^ std::bit_cast<std::uint32_t>(std::round(rect.y))) * 1099511628211ull;
    return h == 0 ? 1 : h;
}

void GameUi::text(std::string_view s, glm::vec4 rect, HudCanvas::Align align, float size, glm::vec4 color) {
    if (hud_ == nullptr || s.empty()) return;
    const float y = rect.y + (rect.w - size * 1.15f) * 0.5f;  // vertically centred line
    const float pad = align == HudCanvas::Align::Left ? 10.0f : align == HudCanvas::Align::Right ? -10.0f : 0.0f;
    hud_->addText({rect.x + pad, y}, size, color, std::string(s), align, rect.z);
}

void GameUi::panel(glm::vec4 rect) {
    if (hud_ != nullptr) hud_->addRect({rect.x, rect.y}, {rect.z, rect.w}, kPanel);
}

void GameUi::label(std::string_view s, glm::vec4 rect, HudCanvas::Align align, glm::vec4 color) {
    text(s, rect, align, style_.fontSize, color);
}

bool GameUi::button(std::string_view label, glm::vec4 rect) {
    const std::uint64_t id = idOf(label, rect);
    const bool focused = takeFocusSlot();
    const bool over = hovered(rect) || focused;
    if (hovered(rect) && pressed(MouseButton::Left)) active_ = id;
    bool clicked = focused && nav_.submit;
    if (released(MouseButton::Left) && active_ == id) {
        clicked = clicked || hovered(rect);
        active_ = 0;
    }
    const bool held = active_ == id && input_ != nullptr && input_->isDown(MouseButton::Left);
    if (hud_ != nullptr) {
        const glm::vec4 fill = held && over ? glm::vec4(style_.accent * 0.8f, 0.97f) : over ? kHover : kButton;
        hud_->addRect({rect.x, rect.y}, {rect.z, rect.w}, fill);
        if (over) hud_->addRect({rect.x, rect.y + rect.w - 3.0f}, {rect.z, 3.0f}, glm::vec4(style_.accent, 1.0f));
    }
    text(label, rect, HudCanvas::Align::Center, style_.fontSize, kText);
    return clicked;
}

bool GameUi::checkbox(std::string_view label, bool value, glm::vec4 rect) {
    const std::uint64_t id = idOf(label, rect);
    const bool focused = takeFocusSlot();
    const bool over = hovered(rect) || focused;
    if (hovered(rect) && pressed(MouseButton::Left)) active_ = id;
    bool clicked = focused && nav_.submit;
    if (released(MouseButton::Left) && active_ == id) {
        clicked = clicked || hovered(rect);
        active_ = 0;
    }
    if (hud_ != nullptr) {
        const float box = std::min(rect.w - 6.0f, 26.0f);
        const glm::vec2 at{rect.x + 4.0f, rect.y + (rect.w - box) * 0.5f};
        hud_->addRect(at, {box, box}, over ? kHover : kButton);
        if (value != clicked) {  // what it shows after this click
            hud_->addRect(at + glm::vec2(5.0f), glm::vec2(box - 10.0f), glm::vec4(style_.accent, 1.0f));
        }
        text(label, {rect.x + box + 4.0f, rect.y, rect.z - box - 4.0f, rect.w}, HudCanvas::Align::Left, style_.fontSize, kText);
    }
    return clicked;
}

bool GameUi::slider(std::string_view label, float& value, float min, float max, glm::vec4 rect) {
    if (max < min) std::swap(min, max);
    const std::uint64_t id = idOf(label, rect);
    const bool focused = takeFocusSlot();
    const bool over = hovered(rect) || focused;
    if (hovered(rect) && pressed(MouseButton::Left)) active_ = id;
    const bool held = active_ == id && input_ != nullptr && (input_->isDown(MouseButton::Left) || pressed(MouseButton::Left));
    bool changed = false;
    if (focused && (nav_.left || nav_.right) && max > min) {
        const float before = value;
        value = std::clamp(value + (nav_.right ? 1.0f : -1.0f) * (max - min) * 0.05f, min, max);
        changed = value != before;
    }
    if (held && rect.z > 0.0f && max > min) {
        const float t = std::clamp((mouse_.x - rect.x) / rect.z, 0.0f, 1.0f);
        const float next = min + t * (max - min);
        changed = next != value;
        value = next;
    }
    if (released(MouseButton::Left) && active_ == id) active_ = 0;
    value = std::clamp(value, min, max);
    if (hud_ != nullptr) {
        const float t = max > min ? (value - min) / (max - min) : 0.0f;
        hud_->addRect({rect.x, rect.y}, {rect.z, rect.w}, over || held ? kHover : kTrack);
        hud_->addRect({rect.x, rect.y}, {rect.z * t, rect.w}, glm::vec4(style_.accent * (held ? 1.0f : 0.75f), 0.95f));
        text(label, rect, HudCanvas::Align::Left, style_.fontSize, kText);
        text(formatValue(value), rect, HudCanvas::Align::Right, style_.fontSize, kText);
    }
    return changed;
}

} // namespace lumora
