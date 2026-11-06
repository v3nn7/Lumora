#include "runtime/InputActions.hpp"

#include "core/FileSystem.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <system_error>

namespace lumora {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(Key::Count)> kKeyNames = {
    "W", "A", "S", "D", "Q", "E", "F", "Up", "Down", "Left", "Right", "Space", "LeftShift", "LeftCtrl", "LeftAlt",
    "Escape", "Enter", "Tab", "Backspace", "B", "C", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "R", "T",
    "U", "V", "X", "Y", "Z", "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9"};
constexpr std::array<const char*, static_cast<std::size_t>(MouseButton::Count)> kMouseNames = {"Left", "Right",
                                                                                                 "Middle"};
constexpr std::array<const char*, static_cast<std::size_t>(GamepadButton::Count)> kPadNames = {
    "A", "B", "X", "Y", "LeftBumper", "RightBumper", "Back", "Start", "Guide", "LeftStick", "RightStick",
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight"};
constexpr std::array<const char*, static_cast<std::size_t>(GamepadAxis::Count)> kAxisNames = {
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};

template <std::size_t N>
int indexOf(const std::array<const char*, N>& names, std::string_view name) {
    for (std::size_t i = 0; i < N; ++i) {
        if (name == names[i]) return static_cast<int>(i);
    }
    return -1;
}

std::vector<InputBinding> bindings(std::initializer_list<const char*> names) {
    std::vector<InputBinding> out;
    for (const char* n : names) out.push_back(*InputBinding::parse(n));
    return out;
}

} // namespace

std::optional<InputBinding> InputBinding::parse(std::string_view name) {
    if (name.starts_with("Mouse.")) {
        const int i = indexOf(kMouseNames, name.substr(6));
        if (i >= 0) return InputBinding{Kind::Mouse, i};
        return std::nullopt;
    }
    if (name.starts_with("Gamepad.")) {
        std::string_view rest = name.substr(8);
        if (const int b = indexOf(kPadNames, rest); b >= 0) return InputBinding{Kind::PadButton, b};
        Kind kind = Kind::PadAxisPositive;
        if (rest.ends_with('-') || rest.ends_with('+')) {
            kind = rest.back() == '-' ? Kind::PadAxisNegative : Kind::PadAxisPositive;
            rest.remove_suffix(1);
        }
        if (const int a = indexOf(kAxisNames, rest); a >= 0) return InputBinding{kind, a};
        return std::nullopt;
    }
    if (const int k = indexOf(kKeyNames, name); k >= 0) return InputBinding{Kind::Key, k};
    return std::nullopt;
}

std::string InputBinding::name() const {
    switch (kind) {
        case Kind::Key: return kKeyNames.at(static_cast<std::size_t>(code));
        case Kind::Mouse: return std::string("Mouse.") + kMouseNames.at(static_cast<std::size_t>(code));
        case Kind::PadButton: return std::string("Gamepad.") + kPadNames.at(static_cast<std::size_t>(code));
        case Kind::PadAxisPositive:
        case Kind::PadAxisNegative: {
            std::string n = std::string("Gamepad.") + kAxisNames.at(static_cast<std::size_t>(code));
            const bool trigger = code >= static_cast<int>(GamepadAxis::LeftTrigger);
            if (!trigger) n += kind == Kind::PadAxisNegative ? "-" : "+";
            return n;
        }
    }
    return {};
}

float InputBinding::value(const InputState& input) const {
    switch (kind) {
        case Kind::Key: return input.isDown(static_cast<Key>(code)) ? 1.0f : 0.0f;
        case Kind::Mouse: return input.isDown(static_cast<MouseButton>(code)) ? 1.0f : 0.0f;
        case Kind::PadButton: return input.isDown(static_cast<GamepadButton>(code)) ? 1.0f : 0.0f;
        case Kind::PadAxisPositive: return std::max(0.0f, input.axis(static_cast<GamepadAxis>(code)));
        case Kind::PadAxisNegative: return std::max(0.0f, -input.axis(static_cast<GamepadAxis>(code)));
    }
    return 0.0f;
}

bool InputBinding::down(const InputState& input) const { return value(input) > 0.5f; }

InputActions InputActions::defaults() {
    InputActions a;
    a.actions_ = {
        {"Jump", bindings({"Space", "Gamepad.A"})},
        {"Fire", bindings({"Mouse.Left", "Gamepad.RightTrigger"})},
        {"Aim", bindings({"Mouse.Right", "Gamepad.LeftTrigger"})},
        {"Interact", bindings({"E", "Gamepad.X"})},
        {"Sprint", bindings({"LeftShift", "Gamepad.LeftStick"})},
        {"Crouch", bindings({"C", "LeftCtrl", "Gamepad.B"})},
        {"Pause", bindings({"Escape", "Gamepad.Start"})},
        {"Submit", bindings({"Enter", "Gamepad.A"})},
        {"Cancel", bindings({"Escape", "Backspace", "Gamepad.B"})},
        {"Up", bindings({"Up", "Gamepad.DPadUp", "Gamepad.LeftY-"})},
        {"Down", bindings({"Down", "Tab", "Gamepad.DPadDown", "Gamepad.LeftY+"})},
        {"Left", bindings({"Left", "Gamepad.DPadLeft", "Gamepad.LeftX-"})},
        {"Right", bindings({"Right", "Gamepad.DPadRight", "Gamepad.LeftX+"})},
    };
    a.axes_ = {
        {"MoveX", Axis{bindings({"A", "Left"}), bindings({"D", "Right"}), GamepadAxis::LeftX, false}},
        {"MoveY", Axis{bindings({"S", "Down"}), bindings({"W", "Up"}), GamepadAxis::LeftY, true}},
        {"LookX", Axis{{}, {}, GamepadAxis::RightX, false}},
        {"LookY", Axis{{}, {}, GamepadAxis::RightY, true}},
    };
    return a;
}

Result<> InputActions::apply(const nlohmann::json& json) {
    if (!json.is_object()) return makeError("input settings must be a JSON object");
    auto readList = [](const nlohmann::json& list, const std::string& where) -> Result<std::vector<InputBinding>> {
        if (!list.is_array()) return makeError("{} must be a list of inputs", where);
        std::vector<InputBinding> out;
        for (const auto& item : list) {
            if (!item.is_string()) return makeError("{}: inputs are names such as \"Space\"", where);
            auto b = InputBinding::parse(item.get<std::string>());
            if (!b) return makeError("{}: unknown input '{}'", where, item.get<std::string>());
            out.push_back(*b);
        }
        return out;
    };
    if (json.contains("actions")) {
        if (!json["actions"].is_object()) return makeError("'actions' must be an object");
        for (const auto& [name, list] : json["actions"].items()) {
            auto parsed = readList(list, "action " + name);
            if (!parsed) return std::unexpected(parsed.error());
            actions_[name] = std::move(*parsed);
        }
    }
    if (json.contains("axes")) {
        if (!json["axes"].is_object()) return makeError("'axes' must be an object");
        for (const auto& [name, def] : json["axes"].items()) {
            if (!def.is_object()) return makeError("axis {} must be an object", name);
            Axis axis;
            if (def.contains("negative")) {
                auto parsed = readList(def["negative"], "axis " + name);
                if (!parsed) return std::unexpected(parsed.error());
                axis.negative = std::move(*parsed);
            }
            if (def.contains("positive")) {
                auto parsed = readList(def["positive"], "axis " + name);
                if (!parsed) return std::unexpected(parsed.error());
                axis.positive = std::move(*parsed);
            }
            if (def.contains("gamepad")) {
                const int a = def["gamepad"].is_string() ? indexOf(kAxisNames, def["gamepad"].get<std::string>()) : -1;
                if (a < 0) return makeError("axis {}: unknown gamepad axis", name);
                axis.gamepad = static_cast<GamepadAxis>(a);
            }
            if (def.contains("invert")) axis.invert = def["invert"].is_boolean() && def["invert"].get<bool>();
            axes_[name] = std::move(axis);
        }
    }
    return {};
}

nlohmann::json InputActions::toJson() const {
    nlohmann::json actions = nlohmann::json::object();
    for (const auto& [name, list] : actions_) {
        nlohmann::json names = nlohmann::json::array();
        for (const InputBinding& b : list) names.push_back(b.name());
        actions[name] = names;
    }
    nlohmann::json axes = nlohmann::json::object();
    for (const auto& [name, axis] : axes_) {
        nlohmann::json def;
        def["negative"] = nlohmann::json::array();
        def["positive"] = nlohmann::json::array();
        for (const InputBinding& b : axis.negative) def["negative"].push_back(b.name());
        for (const InputBinding& b : axis.positive) def["positive"].push_back(b.name());
        if (axis.gamepad) def["gamepad"] = kAxisNames.at(static_cast<std::size_t>(*axis.gamepad));
        def["invert"] = axis.invert;
        axes[name] = def;
    }
    return {{"actions", actions}, {"axes", axes}};
}

Result<InputActions> InputActions::forProject(const std::filesystem::path& projectRoot) {
    InputActions a = defaults();
    const auto file = projectRoot / "Config" / "Input.json";
    std::error_code ec;
    if (projectRoot.empty() || !std::filesystem::is_regular_file(file, ec)) return a;
    auto text = fs::readTextFile(file);
    if (!text) return std::unexpected(text.error());
    const nlohmann::json json = nlohmann::json::parse(*text, nullptr, false);
    if (json.is_discarded()) return makeError("{}: not valid JSON", file.string());
    if (auto applied = a.apply(json); !applied) return makeError("{}: {}", file.string(), applied.error().message);
    return a;
}

void InputActions::beginStep(const InputState* input) {
    input_ = input;
    for (const auto& [name, list] : actions_) {
        const bool now = down(name);
        bool& before = wasDown_[name];
        // A new press of any button counts even while another binding keeps the action down,
        // and a tap between two steps is not lost.
        std::uint64_t presses = 0;
        if (input != nullptr) {
            for (const InputBinding& b : list) {
                switch (b.kind) {
                    case InputBinding::Kind::Key: presses += input->pressCount(static_cast<Key>(b.code)); break;
                    case InputBinding::Kind::Mouse: presses += input->pressCount(static_cast<MouseButton>(b.code)); break;
                    case InputBinding::Kind::PadButton:
                        presses += input->pressCount(static_cast<GamepadButton>(b.code));
                        break;
                    default: break;
                }
            }
        }
        std::uint64_t& seen = seenPresses_[name];
        pressed_[name] = (now && !before) || (primed_ && presses != seen);
        seen = presses;
        before = now;
    }
    primed_ = true;
}

bool InputActions::down(std::string_view action) const {
    if (input_ == nullptr) return false;
    const auto it = actions_.find(action);
    if (it == actions_.end()) return false;
    for (const InputBinding& b : it->second) {
        if (b.down(*input_)) return true;
    }
    return false;
}

bool InputActions::pressed(std::string_view action) const {
    const auto it = pressed_.find(action);
    return it != pressed_.end() && it->second;
}

float InputActions::axis(std::string_view name) const {
    if (input_ == nullptr) return 0.0f;
    const auto it = axes_.find(name);
    if (it == axes_.end()) return 0.0f;
    const Axis& a = it->second;
    float value = 0.0f;
    for (const InputBinding& b : a.positive) value = std::max(value, b.value(*input_));
    float negative = 0.0f;
    for (const InputBinding& b : a.negative) negative = std::max(negative, b.value(*input_));
    value -= negative;
    if (a.gamepad) {
        float pad = input_->axis(*a.gamepad);
        if (a.invert) pad = -pad;
        if (std::abs(pad) > std::abs(value)) value = pad;  // whichever is pushed further
    }
    return std::clamp(value, -1.0f, 1.0f);
}

} // namespace lumora
