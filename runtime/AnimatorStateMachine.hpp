// Animator controllers (.lanim): a state machine that picks the Animator's clip from parameters
// that scripts set (speed, grounded, a jump trigger...), and crossfades between clips.
//
//   { "parameters": [ { "name": "speed", "type": "float", "value": 0 },
//                     { "name": "grounded", "type": "bool", "value": true },
//                     { "name": "jump", "type": "trigger" } ],
//     "entry": "Idle",
//     "states": [ { "name": "Idle", "clip": "Idle", "loop": true, "speed": 1 },
//                 { "name": "Run",  "clip": "Run" },
//                 { "name": "Jump", "clip": "Jump", "loop": false } ],
//     "transitions": [
//       { "from": "Idle", "to": "Run",  "duration": 0.2, "conditions": [ { "parameter": "speed", "op": ">", "value": 0.1 } ] },
//       { "from": "Run",  "to": "Idle", "duration": 0.2, "conditions": [ { "parameter": "speed", "op": "<=", "value": 0.1 } ] },
//       { "from": "*",    "to": "Jump", "duration": 0.1, "conditions": [ { "parameter": "jump", "op": "trigger" } ] },
//       { "from": "Jump", "to": "Idle", "duration": 0.2, "exitTime": 1.0 } ] }
//
// "from": "*" is any state. A transition with exitTime waits until the current clip reached
// that fraction of its length (1 = the end). Ops: > < >= <= == != for numbers, "true" / "false"
// for booleans and "trigger" (consumed when the transition is taken). The first transition whose
// conditions all hold wins, in file order.
#pragma once

#include "core/Error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lumora {

class AnimatorStateMachine {
public:
    enum class ParamType { Float, Bool, Trigger };
    struct State {
        std::string name;
        std::string clip;
        bool loop = true;
        float speed = 1.0f;
    };
    struct Condition {
        std::string parameter;
        std::string op;
        float value = 0.0f;
    };
    struct Transition {
        int from = -1;  // -1: any state
        int to = 0;
        float duration = 0.2f;
        float exitTime = 0.0f;  // 0: no exit time
        std::vector<Condition> conditions;
    };
    /// What the animator should switch to.
    struct Change {
        const State* state;
        float fadeSeconds;
    };

    [[nodiscard]] static Result<AnimatorStateMachine> parse(const nlohmann::json& document);
    [[nodiscard]] static Result<AnimatorStateMachine> load(const std::filesystem::path& file);

    /// The entry state (start of play).
    [[nodiscard]] const State* entry() const noexcept;
    [[nodiscard]] const State* current() const noexcept;

    /// kind 0 float, 1 bool, 2 trigger. False if there is no such parameter.
    bool setParameter(std::string_view name, int kind, float value);
    [[nodiscard]] float parameter(std::string_view name) const;

    /// normalizedTime: how far the current clip is (0..1, may exceed 1 for loops); finished:
    /// a non-looping clip reached its end. Returns the state to switch to, if any.
    [[nodiscard]] std::optional<Change> update(float normalizedTime, bool finished);

    [[nodiscard]] const std::vector<State>& states() const noexcept { return states_; }

private:
    [[nodiscard]] bool holds(const Transition& t, float normalizedTime, bool finished) const;

    struct Param {
        ParamType type = ParamType::Float;
        float value = 0.0f;
    };
    std::unordered_map<std::string, Param> params_;
    std::vector<State> states_;
    std::vector<Transition> transitions_;
    int entry_ = 0;
    int current_ = 0;
};

} // namespace lumora
