#pragma once

#include <lumora/Script.hpp>

#include <array>
#include <string_view>

namespace lumora::script_names {

/// Names of lumora::script::Key in declaration order, as Lua scripts and visual scripts spell them.
inline constexpr std::array<std::string_view, 48> kKeys = {
    "W", "A", "S", "D", "Q", "E", "F",
    "Up", "Down", "Left", "Right",
    "Space", "LeftShift", "LeftCtrl", "LeftAlt", "Escape",
    "Enter", "Tab", "Backspace",
    "B", "C", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "R", "T", "U", "V", "X", "Y", "Z",
    "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
};
static_assert(kKeys.size() == static_cast<std::size_t>(script::Key::Num9) + 1,
              "kKeys must list every lumora::script::Key");

inline constexpr std::array<std::string_view, 3> kMouseButtons = {"Left", "Right", "Middle"};

inline constexpr std::array<std::string_view, 15> kGamepadButtons = {
    "A", "B", "X", "Y", "LeftBumper", "RightBumper", "Back", "Start", "Guide", "LeftStick", "RightStick",
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
};
static_assert(kGamepadButtons.size() == static_cast<std::size_t>(script::GamepadButton::DPadRight) + 1);

inline constexpr std::array<std::string_view, 6> kGamepadAxes = {"LeftX", "LeftY", "RightX", "RightY",
                                                                 "LeftTrigger", "RightTrigger"};
static_assert(kGamepadAxes.size() == static_cast<std::size_t>(script::GamepadAxis::RightTrigger) + 1);

/// Index of `name` in `names`, or -1.
template <std::size_t N>
[[nodiscard]] constexpr int indexOf(const std::array<std::string_view, N>& names, std::string_view name) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

} // namespace lumora::script_names
