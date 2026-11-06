#pragma once

#include "input/InputState.hpp"
#include "scene/Hud.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace lumora {

/// Immediate-mode game UI (menus, settings, buttons over the game) for behaviours. Every step a
/// behaviour calls the widgets it wants to show; each one is drawn into the HUD and reports
/// whether it was used. Clicks are taken from the input's press/release counts, so a click
/// shorter than a frame is never lost and is seen exactly once.
class GameUi {
public:
    struct Style {
        glm::vec3 accent{0.33f, 0.56f, 0.95f};
        float fontSize = 22.0f;
    };

    /// Samples the input for this step; `input` may be null (no clicks).
    void beginStep(const InputState* input, HudCanvas* hud);

    /// Mouse position in HUD pixels.
    [[nodiscard]] glm::vec2 mouse() const noexcept { return mouse_; }
    [[nodiscard]] bool pressed(MouseButton b) const noexcept { return pressed_[static_cast<std::size_t>(b)]; }
    [[nodiscard]] bool released(MouseButton b) const noexcept { return released_[static_cast<std::size_t>(b)]; }
    [[nodiscard]] bool keyPressed(Key key) const noexcept { return keyPressed_[static_cast<std::size_t>(key)]; }

    /// rect = x, y, width, height in HUD pixels.
    bool button(std::string_view label, glm::vec4 rect);
    /// A box with a label; returns true when clicked (the caller flips its value).
    bool checkbox(std::string_view label, bool value, glm::vec4 rect);
    /// Drag along the track to change `value`; returns true while it changes.
    bool slider(std::string_view label, float& value, float min, float max, glm::vec4 rect);
    /// A dark background for a menu.
    void panel(glm::vec4 rect);
    void label(std::string_view text, glm::vec4 rect, HudCanvas::Align align, glm::vec4 color);

    Style& style() noexcept { return style_; }

private:
    [[nodiscard]] bool hovered(glm::vec4 rect) const noexcept;
    [[nodiscard]] static std::uint64_t idOf(std::string_view label, glm::vec4 rect) noexcept;
    void text(std::string_view text, glm::vec4 rect, HudCanvas::Align align, float size, glm::vec4 color);

    HudCanvas* hud_ = nullptr;
    const InputState* input_ = nullptr;
    Style style_;
    glm::vec2 mouse_{-1.0f};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> pressed_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> released_{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> keyPressed_{};
    std::array<std::uint32_t, static_cast<std::size_t>(MouseButton::Count)> seenPresses_{};
    std::array<std::uint32_t, static_cast<std::size_t>(MouseButton::Count)> seenReleases_{};
    std::array<std::uint32_t, static_cast<std::size_t>(Key::Count)> seenKeys_{};
    bool primed_ = false;     // counts captured once (presses before play do not count)
    std::uint64_t active_ = 0;  // widget the left button went down on
};

} // namespace lumora
