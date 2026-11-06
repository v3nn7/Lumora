#pragma once

#include "core/Error.hpp"
#include "input/InputState.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lumora {

/// One physical input: a key, a mouse button, a gamepad button, or a gamepad axis read as a
/// button (a trigger pulled past half, a stick pushed past half one way).
struct InputBinding {
    enum class Kind : std::uint8_t { Key, Mouse, PadButton, PadAxisPositive, PadAxisNegative };
    Kind kind = Kind::Key;
    int code = 0;

    /// "Space", "W", "Num1", "Mouse.Left", "Gamepad.A", "Gamepad.RightTrigger", "Gamepad.LeftX-".
    [[nodiscard]] static std::optional<InputBinding> parse(std::string_view name);
    [[nodiscard]] std::string name() const;
    [[nodiscard]] bool down(const InputState& input) const;
    [[nodiscard]] float value(const InputState& input) const;  // 0..1
    friend bool operator==(const InputBinding&, const InputBinding&) = default;
};

/// Named game inputs, so behaviours ask for "Jump" instead of a key, and players can use a
/// keyboard or a gamepad. Actions are buttons; axes are -1..1 from two sets of buttons and an
/// optional gamepad axis. A project changes or adds them in Config/Input.json:
///
///     { "actions": { "Jump": ["Space", "Gamepad.A"] },
///       "axes": { "MoveX": { "negative": ["A", "Left"], "positive": ["D", "Right"],
///                            "gamepad": "LeftX", "invert": false } } }
class InputActions {
public:
    struct Axis {
        std::vector<InputBinding> negative;
        std::vector<InputBinding> positive;
        std::optional<GamepadAxis> gamepad;
        bool invert = false;
    };

    /// Jump, Fire, Aim, Interact, Sprint, Crouch, Pause, Submit, Cancel, Up, Down, Left, Right;
    /// axes MoveX, MoveY (forward +), LookX, LookY (up +).
    [[nodiscard]] static InputActions defaults();
    /// The defaults with a project's Config/Input.json applied (missing file: defaults).
    [[nodiscard]] static Result<InputActions> forProject(const std::filesystem::path& projectRoot);
    /// Replaces or adds the actions and axes the JSON names.
    [[nodiscard]] Result<> apply(const nlohmann::json& json);
    [[nodiscard]] nlohmann::json toJson() const;

    /// Samples edges for this step (call once per step before asking pressed()).
    void beginStep(const InputState* input);
    [[nodiscard]] bool down(std::string_view action) const;
    /// Went down since the previous step (once per press).
    [[nodiscard]] bool pressed(std::string_view action) const;
    [[nodiscard]] float axis(std::string_view name) const;

    [[nodiscard]] const std::map<std::string, std::vector<InputBinding>, std::less<>>& actions() const noexcept {
        return actions_;
    }
    [[nodiscard]] const std::map<std::string, Axis, std::less<>>& axes() const noexcept { return axes_; }

private:
    std::map<std::string, std::vector<InputBinding>, std::less<>> actions_;
    std::map<std::string, Axis, std::less<>> axes_;
    const InputState* input_ = nullptr;
    std::map<std::string, bool, std::less<>> wasDown_;  // per action, at the previous step
    std::map<std::string, bool, std::less<>> pressed_;
    std::map<std::string, std::uint64_t, std::less<>> seenPresses_;  // digital presses counted so far
    bool primed_ = false;
};

} // namespace lumora
