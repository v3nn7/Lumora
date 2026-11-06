#include "runtime/AnimatorStateMachine.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>

namespace lumora {

Result<AnimatorStateMachine> AnimatorStateMachine::parse(const nlohmann::json& doc) {
    if (!doc.is_object()) return makeError("an animator controller must be a JSON object");
    AnimatorStateMachine m;
    if (const auto it = doc.find("parameters"); it != doc.end() && it->is_array()) {
        for (const auto& p : *it) {
            if (!p.is_object() || !p.contains("name") || !p["name"].is_string()) continue;
            Param param;
            const std::string type = p.value("type", std::string("float"));
            param.type = type == "bool" ? ParamType::Bool : type == "trigger" ? ParamType::Trigger : ParamType::Float;
            if (p.contains("value")) {
                const auto& v = p["value"];
                param.value = v.is_boolean() ? (v.get<bool>() ? 1.0f : 0.0f) : v.is_number() ? v.get<float>() : 0.0f;
            }
            if (param.type == ParamType::Trigger) param.value = 0.0f;
            m.params_[p["name"].get<std::string>()] = param;
        }
    }
    std::unordered_map<std::string, int> index;
    if (const auto it = doc.find("states"); it != doc.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (!s.is_object() || !s.contains("name") || !s["name"].is_string()) continue;
            State state;
            state.name = s["name"].get<std::string>();
            state.clip = s.value("clip", state.name);
            state.loop = s.value("loop", true);
            state.speed = s.value("speed", 1.0f);
            if (index.contains(state.name)) return makeError("animator state '{}' is defined twice", state.name);
            index[state.name] = static_cast<int>(m.states_.size());
            m.states_.push_back(std::move(state));
        }
    }
    if (m.states_.empty()) return makeError("an animator controller needs at least one state");
    if (doc.contains("entry") && doc["entry"].is_string()) {
        const auto it = index.find(doc["entry"].get<std::string>());
        if (it == index.end()) return makeError("entry state '{}' does not exist", doc["entry"].get<std::string>());
        m.entry_ = it->second;
    }
    if (const auto it = doc.find("transitions"); it != doc.end() && it->is_array()) {
        for (const auto& t : *it) {
            if (!t.is_object()) continue;
            Transition tr;
            const std::string from = t.value("from", std::string("*"));
            const std::string to = t.value("to", std::string());
            if (from != "*") {
                const auto f = index.find(from);
                if (f == index.end()) return makeError("transition from unknown state '{}'", from);
                tr.from = f->second;
            }
            const auto target = index.find(to);
            if (target == index.end()) return makeError("transition to unknown state '{}'", to);
            tr.to = target->second;
            tr.duration = std::max(0.0f, t.value("duration", 0.2f));
            tr.exitTime = std::max(0.0f, t.value("exitTime", 0.0f));
            if (const auto c = t.find("conditions"); c != t.end() && c->is_array()) {
                for (const auto& cond : *c) {
                    if (!cond.is_object()) continue;
                    Condition condition;
                    condition.parameter = cond.value("parameter", std::string());
                    condition.op = cond.value("op", std::string("true"));
                    condition.value = cond.contains("value") && cond["value"].is_number() ? cond["value"].get<float>() : 0.0f;
                    if (!m.params_.contains(condition.parameter)) {
                        return makeError("condition uses unknown parameter '{}'", condition.parameter);
                    }
                    tr.conditions.push_back(std::move(condition));
                }
            }
            m.transitions_.push_back(std::move(tr));
        }
    }
    m.current_ = m.entry_;
    return m;
}

Result<AnimatorStateMachine> AnimatorStateMachine::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return makeError("cannot open {}", file.string());
    const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded()) return makeError("{} is not valid JSON", file.string());
    auto machine = parse(doc);
    if (!machine) return makeError("{}: {}", file.filename().string(), machine.error().message);
    return machine;
}

const AnimatorStateMachine::State* AnimatorStateMachine::entry() const noexcept { return &states_[static_cast<std::size_t>(entry_)]; }
const AnimatorStateMachine::State* AnimatorStateMachine::current() const noexcept {
    return &states_[static_cast<std::size_t>(current_)];
}

bool AnimatorStateMachine::setParameter(std::string_view name, int kind, float value) {
    const auto it = params_.find(std::string(name));
    if (it == params_.end()) return false;
    if (kind == 2 || it->second.type == ParamType::Trigger) {
        it->second.value = 1.0f;
    } else if (kind == 1 || it->second.type == ParamType::Bool) {
        it->second.value = value != 0.0f ? 1.0f : 0.0f;
    } else {
        it->second.value = value;
    }
    return true;
}

float AnimatorStateMachine::parameter(std::string_view name) const {
    const auto it = params_.find(std::string(name));
    return it != params_.end() ? it->second.value : 0.0f;
}

bool AnimatorStateMachine::holds(const Transition& t, float normalizedTime, bool finished) const {
    if (t.from >= 0 && t.from != current_) return false;
    if (t.from < 0 && t.to == current_) return false;  // any-state transitions never re-enter their target
    if (t.exitTime > 0.0f && !finished && normalizedTime < t.exitTime) return false;
    for (const Condition& c : t.conditions) {
        const float v = params_.at(c.parameter).value;
        bool ok = false;
        if (c.op == ">") ok = v > c.value;
        else if (c.op == "<") ok = v < c.value;
        else if (c.op == ">=") ok = v >= c.value;
        else if (c.op == "<=") ok = v <= c.value;
        else if (c.op == "==") ok = std::abs(v - c.value) < 1e-5f;
        else if (c.op == "!=") ok = std::abs(v - c.value) >= 1e-5f;
        else if (c.op == "false") ok = v == 0.0f;
        else ok = v != 0.0f;  // "true", "trigger"
        if (!ok) return false;
    }
    return true;
}

std::optional<AnimatorStateMachine::Change> AnimatorStateMachine::update(float normalizedTime, bool finished) {
    for (const Transition& t : transitions_) {
        if (!holds(t, normalizedTime, finished)) continue;
        for (const Condition& c : t.conditions) {
            auto& p = params_.at(c.parameter);
            if (p.type == ParamType::Trigger) p.value = 0.0f;  // consumed
        }
        current_ = t.to;
        return Change{current(), t.duration};
    }
    return std::nullopt;
}

} // namespace lumora
