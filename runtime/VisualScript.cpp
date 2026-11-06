#include "runtime/VisualScript.hpp"

#include "runtime/ScriptNames.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <random>

namespace lumora::visual {

using script::Entity;
using script::kNoEntity;
using script::Vec3;

// ---- Values ---------------------------------------------------------------------------------

std::string_view pinTypeName(PinType type) noexcept {
    switch (type) {
    case PinType::Exec: return "exec";
    case PinType::Number: return "number";
    case PinType::Bool: return "bool";
    case PinType::Vec3: return "vec3";
    case PinType::String: return "string";
    case PinType::Entity: return "entity";
    case PinType::Any: return "any";
    }
    return "any";
}

PinType pinTypeFromName(std::string_view name) noexcept {
    if (name == "number") return PinType::Number;
    if (name == "bool") return PinType::Bool;
    if (name == "vec3") return PinType::Vec3;
    if (name == "string") return PinType::String;
    if (name == "entity") return PinType::Entity;
    return PinType::Any;
}

double Value::asNumber() const {
    switch (type) {
    case PinType::Number:
    case PinType::Bool: return number;
    case PinType::Vec3: return vec.length();
    case PinType::String: {
        char* end = nullptr;
        const double n = std::strtod(text.c_str(), &end);
        return end != text.c_str() ? n : 0.0;
    }
    case PinType::Entity: return entity == kNoEntity ? -1.0 : static_cast<double>(entity);
    default: return number;
    }
}

bool Value::asBool() const {
    switch (type) {
    case PinType::String: return !text.empty() && text != "false" && text != "0";
    case PinType::Entity: return entity != kNoEntity;
    case PinType::Vec3: return vec.x != 0.0f || vec.y != 0.0f || vec.z != 0.0f;
    default: return number != 0.0;
    }
}

Vec3 Value::asVec3() const {
    if (type == PinType::Vec3) return vec;
    if (type == PinType::String) {
        Vec3 v;
        if (std::sscanf(text.c_str(), "%f , %f , %f", &v.x, &v.y, &v.z) == 3) return v;
        return {};
    }
    const auto n = static_cast<float>(asNumber());
    return {n, n, n};
}

namespace {
std::string formatNumber(double n) {
    if (std::isfinite(n) && n == std::floor(n) && std::abs(n) < 1e15) return std::format("{}", static_cast<long long>(n));
    return std::format("{:.3f}", n);
}
} // namespace

std::string Value::asString() const {
    switch (type) {
    case PinType::String: return text;
    case PinType::Bool: return number != 0.0 ? "true" : "false";
    case PinType::Vec3: return std::format("({}, {}, {})", formatNumber(vec.x), formatNumber(vec.y), formatNumber(vec.z));
    case PinType::Entity: return entity == kNoEntity ? std::string("none") : std::format("#{}", entity);
    default: return formatNumber(number);
    }
}

Value Value::fromJson(const nlohmann::json& j, PinType type) {
    switch (type) {
    case PinType::Number: return ofNumber(j.is_number() ? j.get<double>() : j.is_boolean() ? (j.get<bool>() ? 1.0 : 0.0) : 0.0);
    case PinType::Bool: return ofBool(j.is_boolean() ? j.get<bool>() : j.is_number() ? j.get<double>() != 0.0 : false);
    case PinType::Vec3: {
        Vec3 v;
        if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
            v = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
        } else if (j.is_number()) {
            const auto n = j.get<float>();
            v = {n, n, n};
        }
        return ofVec3(v);
    }
    case PinType::String: return ofString(j.is_string() ? j.get<std::string>() : j.is_null() ? std::string() : j.dump());
    case PinType::Entity: {
        Value v = ofEntity(kNoEntity);
        if (j.is_string()) v.text = j.get<std::string>();  // entity name, looked up when read
        return v;
    }
    case PinType::Any:
    case PinType::Exec: break;
    }
    if (j.is_boolean()) return ofBool(j.get<bool>());
    if (j.is_number()) return ofNumber(j.get<double>());
    if (j.is_array() && j.size() == 3) return fromJson(j, PinType::Vec3);
    if (j.is_string()) return ofString(j.get<std::string>());
    return ofNumber(0.0);
}

nlohmann::json Value::toJson() const {
    switch (type) {
    case PinType::Bool: return number != 0.0;
    case PinType::Vec3: return nlohmann::json::array({vec.x, vec.y, vec.z});
    case PinType::String: return text;
    case PinType::Entity: return text;
    default: return number;
    }
}

int NodeDef::inputIndex(std::string_view pin) const noexcept {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].name == pin) return static_cast<int>(i);
    }
    return -1;
}

int NodeDef::outputIndex(std::string_view pin) const noexcept {
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].name == pin) return static_cast<int>(i);
    }
    return -1;
}

// ---- Node library -----------------------------------------------------------------------------

namespace {

PinDef execPin(std::string_view name, std::string_view label = {}) { return {name, PinType::Exec, label}; }
PinDef num(std::string_view name, double def = 0.0, std::string_view label = {}) {
    return {name, PinType::Number, label, def};
}
PinDef boolean(std::string_view name, bool def = false) { return {name, PinType::Bool, {}, def ? 1.0 : 0.0}; }
PinDef vec(std::string_view name, std::string_view def = "0,0,0") { return {name, PinType::Vec3, {}, 0.0, def}; }
PinDef str(std::string_view name, std::string_view def = {}, PinHint hint = PinHint::None) {
    return {name, PinType::String, {}, 0.0, def, hint};
}
PinDef ent(std::string_view name = "entity") { return {name, PinType::Entity}; }
PinDef any(std::string_view name) { return {name, PinType::Any}; }

using Fn = std::function<void(Exec&)>;

/// Pure node: outputs computed from inputs on demand.
NodeDef pure(std::string_view type, std::string_view title, std::string_view category, std::string_view description,
             std::vector<PinDef> inputs, std::vector<PinDef> outputs, Fn fn) {
    NodeDef d{type, title, category, description, std::move(inputs), std::move(outputs)};
    d.pure = true;
    d.run = std::move(fn);
    return d;
}

/// Action node: exec in → side effect → exec "then". Data pins start at index 1.
NodeDef action(std::string_view type, std::string_view title, std::string_view category, std::string_view description,
               std::vector<PinDef> inputs, std::vector<PinDef> outputs, Fn fn) {
    inputs.insert(inputs.begin(), execPin("in"));
    outputs.insert(outputs.begin(), execPin("then"));
    NodeDef d{type, title, category, description, std::move(inputs), std::move(outputs)};
    d.run = [fn = std::move(fn)](Exec& x) {
        fn(x);
        x.fire(0);
    };
    return d;
}

/// Flow node: exec in; the function decides which exec outputs run.
NodeDef flow(std::string_view type, std::string_view title, std::string_view description, std::vector<PinDef> inputs,
             std::vector<PinDef> outputs, Fn fn) {
    inputs.insert(inputs.begin(), execPin("in"));
    NodeDef d{type, title, "Flow", description, std::move(inputs), std::move(outputs)};
    d.run = std::move(fn);
    return d;
}

/// Event node: inputs are settings (literals), outputs[0] is the exec pin, then event data.
NodeDef event(std::string_view type, std::string_view title, std::string_view description, std::vector<PinDef> inputs,
              std::vector<PinDef> outputs) {
    outputs.insert(outputs.begin(), execPin("exec", " "));
    NodeDef d{type, title, "Events", description, std::move(inputs), std::move(outputs)};
    d.event = true;
    d.run = [](Exec&) {};
    return d;
}

Vec3 getTransform(const Exec& x, Entity e, int which) {
    float v[3] = {0.0f, 0.0f, 0.0f};
    if (which == 2) v[0] = v[1] = v[2] = 1.0f;
    x.api().getTransform(x.ctx(), e, which, v);
    return {v[0], v[1], v[2]};
}
void setTransform(const Exec& x, Entity e, int which, Vec3 value) {
    const float v[3] = {value.x, value.y, value.z};
    x.api().setTransform(x.ctx(), e, which, v);
}
Vec3 normalize(Vec3 v) {
    const float l = v.length();
    return l > 1e-8f ? v * (1.0f / l) : Vec3{};
}
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

std::mt19937& rng() {
    static thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

void binaryMath(std::vector<NodeDef>& lib, std::string_view type, std::string_view title, std::string_view description,
                double (*op)(double, double), double defB = 0.0) {
    lib.push_back(pure(type, title, "Math", description, {num("a"), num("b", defB)}, {num("result")},
                       [op](Exec& x) { x.out(0, Value::ofNumber(op(x.number(0), x.number(1)))); }));
}
void compare(std::vector<NodeDef>& lib, std::string_view type, std::string_view title, bool (*op)(double, double)) {
    lib.push_back(pure(type, title, "Logic", "Compares two numbers.", {num("a"), num("b")}, {boolean("result")},
                       [op](Exec& x) { x.out(0, Value::ofBool(op(x.number(0), x.number(1)))); }));
}

std::vector<NodeDef> buildLibrary() {
    std::vector<NodeDef> lib;

    // -- Events
    lib.push_back(event("event.start", "On Start", "Runs once when play begins.", {}, {}));
    lib.push_back(event("event.update", "On Update", "Runs every simulation step.", {}, {num("dt", 0.0, "delta time")}));
    lib.push_back(event("event.collision", "On Collision", "This entity started touching another one.", {},
                        {ent("other")}));
    lib.push_back(event("event.key", "On Key Pressed", "A key went down this step.", {str("key", "Space", PinHint::Key)}, {}));
    lib.push_back(event("event.action", "On Action Pressed", "An input action (Config/Input.json) was pressed.",
                        {str("action", "Jump")}, {}));
    lib.push_back(event("event.every", "Every N Seconds", "Runs repeatedly at a fixed interval.", {num("seconds", 1.0)}, {}));
    lib.push_back(event("event.timeline", "On Timeline Event", "A timeline passed an event marker with this name.",
                        {str("name")}, {}));

    // -- Flow
    lib.push_back(flow("flow.branch", "Branch", "Runs True or False depending on the condition.", {boolean("condition")},
                       {execPin("true"), execPin("false")},
                       [](Exec& x) { x.fire(x.flag(1) ? 0 : 1); }));
    lib.push_back(flow("flow.sequence", "Sequence", "Runs each output in order.", {},
                       {execPin("then0", "1"), execPin("then1", "2"), execPin("then2", "3"), execPin("then3", "4")},
                       [](Exec& x) {
                           for (int i = 0; i < 4; ++i) x.fire(i);
                       }));
    lib.push_back(flow("flow.for", "For Loop", "Runs Body for each index from First to Last (inclusive).",
                       {num("first", 0.0), num("last", 9.0)}, {execPin("body"), num("index"), execPin("completed")},
                       [](Exec& x) {
                           const auto first = static_cast<long long>(x.number(1));
                           const auto last = static_cast<long long>(x.number(2));
                           for (long long i = first; i <= last && i - first < 100000; ++i) {
                               x.out(1, Value::ofNumber(static_cast<double>(i)));
                               x.fire(0);
                           }
                           x.fire(2);
                       }));
    lib.push_back(flow("flow.doOnce", "Do Once", "Lets execution through only the first time (until Reset).",
                       {boolean("reset")}, {execPin("then")}, [](Exec& x) {
                           if (x.flag(1)) x.state() = 0.0;
                           if (x.state() != 0.0) return;
                           x.state() = 1.0;
                           x.fire(0);
                       }));
    lib.push_back(flow("flow.flipFlop", "Flip Flop", "Alternates between A and B.", {}, {execPin("a"), execPin("b"), boolean("isA")},
                       [](Exec& x) {
                           const bool a = x.state() == 0.0;
                           x.state() = a ? 1.0 : 0.0;
                           x.out(2, Value::ofBool(a));
                           x.fire(a ? 0 : 1);
                       }));
    lib.push_back(flow("flow.delay", "Delay", "Runs Completed after a number of seconds.", {num("seconds", 1.0)},
                       {execPin("completed")}, [](Exec& x) { x.fireLater(0, static_cast<float>(x.number(1))); }));
    lib.push_back(flow("flow.gate", "Gate", "Lets execution through while Open is true.", {boolean("open", true)},
                       {execPin("then")}, [](Exec& x) {
                           if (x.flag(1)) x.fire(0);
                       }));

    // -- Variables
    lib.push_back(pure("var.get", "Get Variable", "Variables", "Reads a graph variable.",
                       {str("name", {}, PinHint::Variable)}, {any("value")},
                       [](Exec& x) { x.out(0, x.instance().variable(x.text(0))); }));
    lib.push_back(action("var.set", "Set Variable", "Variables", "Writes a graph variable.",
                         {str("name", {}, PinHint::Variable), any("value")}, {any("value")}, [](Exec& x) {
                             Value v = x.in(2);
                             Value& slot = x.instance().variable(x.text(1));
                             // Keep the variable's declared type.
                             switch (slot.type) {
                             case PinType::Number: v = Value::ofNumber(v.asNumber()); break;
                             case PinType::Bool: v = Value::ofBool(v.asBool()); break;
                             case PinType::Vec3: v = Value::ofVec3(v.asVec3()); break;
                             case PinType::String: v = Value::ofString(v.asString()); break;
                             default: break;
                             }
                             slot = v;
                             x.out(1, v);
                         }));
    lib.push_back(action("var.add", "Add To Variable", "Variables", "Adds a number to a number variable.",
                         {str("name", {}, PinHint::Variable), num("amount", 1.0)}, {num("value")}, [](Exec& x) {
                             Value& slot = x.instance().variable(x.text(1));
                             slot = Value::ofNumber(slot.asNumber() + x.number(2));
                             x.out(1, slot);
                         }));

    // -- Math
    binaryMath(lib, "math.add", "Add", "a + b", [](double a, double b) { return a + b; });
    binaryMath(lib, "math.sub", "Subtract", "a - b", [](double a, double b) { return a - b; });
    binaryMath(lib, "math.mul", "Multiply", "a * b", [](double a, double b) { return a * b; }, 1.0);
    binaryMath(lib, "math.div", "Divide", "a / b (0 when b is 0)", [](double a, double b) { return b != 0.0 ? a / b : 0.0; }, 1.0);
    binaryMath(lib, "math.mod", "Modulo", "Remainder of a / b", [](double a, double b) { return b != 0.0 ? std::fmod(a, b) : 0.0; }, 1.0);
    binaryMath(lib, "math.min", "Min", "Smaller of a and b", [](double a, double b) { return std::min(a, b); });
    binaryMath(lib, "math.max", "Max", "Larger of a and b", [](double a, double b) { return std::max(a, b); });
    binaryMath(lib, "math.pow", "Power", "a to the power b", [](double a, double b) { return std::pow(a, b); }, 2.0);
    auto unary = [&](std::string_view type, std::string_view title, double (*op)(double)) {
        lib.push_back(pure(type, title, "Math", title, {num("value")}, {num("result")},
                           [op](Exec& x) { x.out(0, Value::ofNumber(op(x.number(0)))); }));
    };
    unary("math.abs", "Absolute", [](double v) { return std::abs(v); });
    unary("math.negate", "Negate", [](double v) { return -v; });
    unary("math.sqrt", "Square Root", [](double v) { return v > 0.0 ? std::sqrt(v) : 0.0; });
    unary("math.sin", "Sin (radians)", [](double v) { return std::sin(v); });
    unary("math.cos", "Cos (radians)", [](double v) { return std::cos(v); });
    unary("math.floor", "Floor", [](double v) { return std::floor(v); });
    unary("math.round", "Round", [](double v) { return std::round(v); });
    lib.push_back(pure("math.number", "Number", "Math", "A constant number.", {num("value")}, {num("value")},
                       [](Exec& x) { x.out(0, Value::ofNumber(x.number(0))); }));
    lib.push_back(pure("math.lerp", "Lerp", "Math", "Blends from a to b by t (0..1).", {num("a"), num("b", 1.0), num("t", 0.5)},
                       {num("result")}, [](Exec& x) {
                           const double a = x.number(0);
                           x.out(0, Value::ofNumber(a + (x.number(1) - a) * x.number(2)));
                       }));
    lib.push_back(pure("math.clamp", "Clamp", "Math", "Keeps a value between min and max.",
                       {num("value"), num("min", 0.0), num("max", 1.0)}, {num("result")}, [](Exec& x) {
                           const double lo = x.number(1);
                           const double hi = std::max(lo, x.number(2));
                           x.out(0, Value::ofNumber(std::clamp(x.number(0), lo, hi)));
                       }));
    lib.push_back(pure("math.random", "Random Range", "Math", "A random number between min and max (new each read).",
                       {num("min", 0.0), num("max", 1.0)}, {num("result")}, [](Exec& x) {
                           const double lo = x.number(0);
                           const double hi = x.number(1);
                           std::uniform_real_distribution<double> d(std::min(lo, hi), std::max(lo, hi));
                           x.out(0, Value::ofNumber(lo == hi ? lo : d(rng())));
                       }));

    // -- Logic
    compare(lib, "logic.less", "Less (a < b)", [](double a, double b) { return a < b; });
    compare(lib, "logic.lessEq", "Less or Equal (a <= b)", [](double a, double b) { return a <= b; });
    compare(lib, "logic.greater", "Greater (a > b)", [](double a, double b) { return a > b; });
    compare(lib, "logic.greaterEq", "Greater or Equal (a >= b)", [](double a, double b) { return a >= b; });
    compare(lib, "logic.equal", "Equal (a == b)", [](double a, double b) { return std::abs(a - b) < 1e-6; });
    compare(lib, "logic.notEqual", "Not Equal (a != b)", [](double a, double b) { return std::abs(a - b) >= 1e-6; });
    lib.push_back(pure("logic.and", "And", "Logic", "True if both are true.", {boolean("a"), boolean("b")}, {boolean("result")},
                       [](Exec& x) { x.out(0, Value::ofBool(x.flag(0) && x.flag(1))); }));
    lib.push_back(pure("logic.or", "Or", "Logic", "True if either is true.", {boolean("a"), boolean("b")}, {boolean("result")},
                       [](Exec& x) { x.out(0, Value::ofBool(x.flag(0) || x.flag(1))); }));
    lib.push_back(pure("logic.not", "Not", "Logic", "Inverts a boolean.", {boolean("value")}, {boolean("result")},
                       [](Exec& x) { x.out(0, Value::ofBool(!x.flag(0))); }));
    lib.push_back(pure("logic.select", "Select", "Logic", "Picks A when the condition is true, else B.",
                       {boolean("condition"), any("a"), any("b")}, {any("result")},
                       [](Exec& x) { x.out(0, x.flag(0) ? x.in(1) : x.in(2)); }));

    // -- Vectors
    lib.push_back(pure("vec.make", "Make Vector", "Vector", "Builds a vector from x, y, z.", {num("x"), num("y"), num("z")},
                       {vec("vector")}, [](Exec& x) {
                           x.out(0, Value::ofVec3({static_cast<float>(x.number(0)), static_cast<float>(x.number(1)),
                                                   static_cast<float>(x.number(2))}));
                       }));
    lib.push_back(pure("vec.break", "Break Vector", "Vector", "Splits a vector into x, y, z.", {vec("vector")},
                       {num("x"), num("y"), num("z")}, [](Exec& x) {
                           const Vec3 v = x.vec(0);
                           x.out(0, Value::ofNumber(v.x));
                           x.out(1, Value::ofNumber(v.y));
                           x.out(2, Value::ofNumber(v.z));
                       }));
    lib.push_back(pure("vec.add", "Add Vectors", "Vector", "a + b", {vec("a"), vec("b")}, {vec("result")},
                       [](Exec& x) { x.out(0, Value::ofVec3(x.vec(0) + x.vec(1))); }));
    lib.push_back(pure("vec.sub", "Subtract Vectors", "Vector", "a - b", {vec("a"), vec("b")}, {vec("result")},
                       [](Exec& x) { x.out(0, Value::ofVec3(x.vec(0) - x.vec(1))); }));
    lib.push_back(pure("vec.scale", "Scale Vector", "Vector", "vector * number", {vec("vector"), num("scale", 1.0)},
                       {vec("result")}, [](Exec& x) { x.out(0, Value::ofVec3(x.vec(0) * static_cast<float>(x.number(1)))); }));
    lib.push_back(pure("vec.length", "Vector Length", "Vector", "Length of a vector.", {vec("vector")}, {num("length")},
                       [](Exec& x) { x.out(0, Value::ofNumber(x.vec(0).length())); }));
    lib.push_back(pure("vec.normalize", "Normalize", "Vector", "Same direction, length 1.", {vec("vector")}, {vec("result")},
                       [](Exec& x) { x.out(0, Value::ofVec3(normalize(x.vec(0)))); }));
    lib.push_back(pure("vec.dot", "Dot Product", "Vector", "a · b", {vec("a"), vec("b")}, {num("result")},
                       [](Exec& x) { x.out(0, Value::ofNumber(dot(x.vec(0), x.vec(1)))); }));
    lib.push_back(pure("vec.cross", "Cross Product", "Vector", "a × b", {vec("a"), vec("b")}, {vec("result")},
                       [](Exec& x) { x.out(0, Value::ofVec3(cross(x.vec(0), x.vec(1)))); }));
    lib.push_back(pure("vec.distance", "Distance", "Vector", "Distance between two points.", {vec("a"), vec("b")},
                       {num("distance")}, [](Exec& x) { x.out(0, Value::ofNumber((x.vec(0) - x.vec(1)).length())); }));
    lib.push_back(pure("vec.lerp", "Lerp Vectors", "Vector", "Blends from a to b by t (0..1).",
                       {vec("a"), vec("b"), num("t", 0.5)}, {vec("result")}, [](Exec& x) {
                           const Vec3 a = x.vec(0);
                           x.out(0, Value::ofVec3(a + (x.vec(1) - a) * static_cast<float>(x.number(2))));
                       }));

    // -- Text
    lib.push_back(pure("text.concat", "Join Text", "Text", "Joins a and b.", {any("a"), any("b")}, {str("text")},
                       [](Exec& x) { x.out(0, Value::ofString(x.in(0).asString() + x.in(1).asString())); }));
    lib.push_back(pure("text.number", "Number To Text", "Text", "Formats a number with a number of decimals.",
                       {num("value"), num("decimals", 0.0)}, {str("text")}, [](Exec& x) {
                           const int d = std::clamp(static_cast<int>(x.number(1)), 0, 6);
                           x.out(0, Value::ofString(std::format("{:.{}f}", x.number(0), d)));
                       }));
    lib.push_back(pure("text.translate", "Translate", "Text", "Localized text for a key (Localization/*.json).",
                       {str("key")}, {str("text")}, [](Exec& x) {
                           const std::string key = x.text(0);
                           char buffer[1024];
                           const int n = x.api().localize(x.ctx(), 0, key.c_str(), buffer, sizeof buffer);
                           x.out(0, Value::ofString(n >= 0 ? std::string(buffer, static_cast<std::size_t>(
                                                                                    std::min<int>(n, sizeof buffer - 1)))
                                                           : key));
                       }));

    // -- Entities
    lib.push_back(pure("entity.self", "Self", "Entity", "The entity this graph runs on.", {}, {ent()},
                       [](Exec& x) { x.out(0, Value::ofEntity(x.self())); }));
    lib.push_back(pure("entity.find", "Find Entity", "Entity", "The first entity with this name (none if missing).",
                       {str("name")}, {ent(), boolean("found")}, [](Exec& x) {
                           const std::string name = x.text(0);
                           const Entity e = x.api().find(x.ctx(), name.c_str());
                           x.out(0, Value::ofEntity(e));
                           x.out(1, Value::ofBool(e != kNoEntity));
                       }));
    lib.push_back(pure("entity.name", "Entity Name", "Entity", "Name of an entity.", {ent()}, {str("name")}, [](Exec& x) {
        const char* n = x.api().name(x.ctx(), x.entity(0));
        x.out(0, Value::ofString(n != nullptr ? n : ""));
    }));
    lib.push_back(pure("entity.valid", "Is Valid", "Entity", "The entity still exists.", {ent()}, {boolean("valid")},
                       [](Exec& x) {
                           float v[3];
                           x.out(0, Value::ofBool(x.api().getTransform(x.ctx(), x.entity(0), 0, v) != 0));
                       }));
    const std::pair<std::string_view, int> transforms[] = {{"Position", 0}, {"Rotation", 1}, {"Scale", 2}};
    for (const auto& [label, which] : transforms) {
        static const std::string_view getTypes[] = {"entity.getPosition", "entity.getRotation", "entity.getScale"};
        static const std::string_view getTitles[] = {"Get Position", "Get Rotation", "Get Scale"};
        static const std::string_view setTypes[] = {"entity.setPosition", "entity.setRotation", "entity.setScale"};
        static const std::string_view setTitles[] = {"Set Position", "Set Rotation", "Set Scale"};
        const int w = which;
        (void)label;
        lib.push_back(pure(getTypes[w], getTitles[w], "Entity", "Reads the entity's transform (rotation in degrees).",
                           {ent()}, {vec("value")}, [w](Exec& x) { x.out(0, Value::ofVec3(getTransform(x, x.entity(0), w))); }));
        lib.push_back(action(setTypes[w], setTitles[w], "Entity", "Writes the entity's transform (rotation in degrees).",
                             {ent(), vec("value", w == 2 ? "1,1,1" : "0,0,0")}, {},
                             [w](Exec& x) { setTransform(x, x.entity(1), w, x.vec(2)); }));
    }
    lib.push_back(action("entity.translate", "Move By", "Entity", "Adds an offset to the position.",
                         {ent(), vec("offset")}, {}, [](Exec& x) {
                             const Entity e = x.entity(1);
                             setTransform(x, e, 0, getTransform(x, e, 0) + x.vec(2));
                         }));
    lib.push_back(action("entity.rotate", "Rotate By", "Entity", "Adds degrees to the rotation.", {ent(), vec("degrees")}, {},
                         [](Exec& x) {
                             const Entity e = x.entity(1);
                             setTransform(x, e, 1, getTransform(x, e, 1) + x.vec(2));
                         }));
    lib.push_back(action("entity.destroy", "Destroy", "Entity", "Removes the entity at the end of the step.", {ent()}, {},
                         [](Exec& x) { x.api().destroy(x.ctx(), x.entity(1)); }));
    lib.push_back(action("entity.clone", "Clone", "Entity", "Copies an entity with its children and components.", {ent()},
                         {ent("clone")}, [](Exec& x) { x.out(1, Value::ofEntity(x.api().clone(x.ctx(), x.entity(1)))); }));
    lib.push_back(action("entity.spawn", "Spawn Prefab", "Entity", "Places a prefab (.lprefab) at a position.",
                         {str("prefab", "Content/Prefabs/", PinHint::Asset), vec("position")}, {ent("spawned")},
                         [](Exec& x) {
                             const std::string prefab = x.text(1);
                             const Vec3 p = x.vec(2);
                             const float xyz[3] = {p.x, p.y, p.z};
                             x.out(1, Value::ofEntity(x.api().spawn(x.ctx(), prefab.c_str(), xyz)));
                         }));
    lib.push_back(pure("entity.param", "Script Parameter", "Entity", "A number from the Script component's Parameters.",
                       {str("name"), num("fallback")}, {num("value")}, [](Exec& x) {
                           const std::string name = x.text(0);
                           x.out(0, Value::ofNumber(x.api().parameter(x.ctx(), x.self(), name.c_str(), x.number(1))));
                       }));

    // -- Physics
    auto push = [](Exec& x, int impulse) {
        const Vec3 f = x.vec(2);
        const float xyz[3] = {f.x, f.y, f.z};
        x.api().addForce(x.ctx(), x.entity(1), xyz, impulse);
    };
    lib.push_back(action("physics.addForce", "Add Force", "Physics", "Pushes a dynamic body (continuous).",
                         {ent(), vec("force")}, {}, [push](Exec& x) { push(x, 0); }));
    lib.push_back(action("physics.addImpulse", "Add Impulse", "Physics", "Kicks a dynamic body (instant).",
                         {ent(), vec("impulse")}, {}, [push](Exec& x) { push(x, 1); }));
    lib.push_back(action("physics.setVelocity", "Set Velocity", "Physics", "Sets a dynamic body's velocity.",
                         {ent(), vec("velocity")}, {}, [](Exec& x) {
                             const Vec3 v = x.vec(2);
                             const float xyz[3] = {v.x, v.y, v.z};
                             x.api().setVelocity(x.ctx(), x.entity(1), xyz);
                         }));
    lib.push_back(pure("physics.getVelocity", "Get Velocity", "Physics", "A dynamic body's velocity.", {ent()},
                       {vec("velocity")}, [](Exec& x) {
                           float v[3] = {0, 0, 0};
                           x.api().getVelocity(x.ctx(), x.entity(0), v);
                           x.out(0, Value::ofVec3({v[0], v[1], v[2]}));
                       }));
    lib.push_back(action("physics.raycast", "Raycast", "Physics", "Casts a ray against colliders.",
                         {vec("origin"), vec("direction", "0,-1,0"), num("maxDistance", 100.0)},
                         {boolean("hit"), ent("entity"), vec("point"), vec("normal"), num("distance")}, [](Exec& x) {
                             const Vec3 o = x.vec(1);
                             const Vec3 d = normalize(x.vec(2));
                             const float origin[3] = {o.x, o.y, o.z};
                             const float dir[3] = {d.x, d.y, d.z};
                             script::RayHit hit;
                             const bool ok = x.api().raycast(x.ctx(), origin, dir, static_cast<float>(x.number(3)), &hit) != 0;
                             x.out(1, Value::ofBool(ok));
                             x.out(2, Value::ofEntity(ok ? hit.entity : kNoEntity));
                             x.out(3, Value::ofVec3(hit.point));
                             x.out(4, Value::ofVec3(hit.normal));
                             x.out(5, Value::ofNumber(hit.distance));
                         }));

    // -- Character
    lib.push_back(action("character.move", "Character Move", "Character", "Walks the Character Controller (m/s).",
                         {vec("velocity")}, {}, [](Exec& x) {
                             const Vec3 v = x.vec(1);
                             const float xyz[3] = {v.x, v.y, v.z};
                             x.api().character(x.ctx(), x.self(), 0, xyz);
                         }));
    lib.push_back(action("character.jump", "Character Jump", "Character", "Jumps if the character is on the ground.",
                         {num("speed", 6.0)}, {boolean("jumped")}, [](Exec& x) {
                             const float xyz[3] = {static_cast<float>(x.number(1)), 0, 0};
                             x.out(1, Value::ofBool(x.api().character(x.ctx(), x.self(), 1, xyz) != 0));
                         }));
    lib.push_back(pure("character.grounded", "Is Grounded", "Character", "The character stands on something.", {},
                       {boolean("grounded")},
                       [](Exec& x) { x.out(0, Value::ofBool(x.api().character(x.ctx(), x.self(), 2, nullptr) != 0)); }));

    // -- Input
    lib.push_back(pure("input.keyDown", "Key Down", "Input", "The key is held.", {str("key", "W", PinHint::Key)},
                       {boolean("down")}, [](Exec& x) {
                           const int k = script_names::indexOf(script_names::kKeys, x.text(0));
                           x.out(0, Value::ofBool(k >= 0 && x.api().keyDown(x.ctx(), k) != 0));
                       }));
    lib.push_back(pure("input.keyPressed", "Key Pressed", "Input", "The key went down this step.",
                       {str("key", "Space", PinHint::Key)}, {boolean("pressed")}, [](Exec& x) {
                           const int k = script_names::indexOf(script_names::kKeys, x.text(0));
                           x.out(0, Value::ofBool(k >= 0 && x.api().inputEdge(x.ctx(), 2, k) != 0));
                       }));
    lib.push_back(pure("input.action", "Action Down", "Input", "An input action is held.", {str("action", "Jump")},
                       {boolean("down"), boolean("pressed")}, [](Exec& x) {
                           const std::string name = x.text(0);
                           x.out(0, Value::ofBool(x.api().input(x.ctx(), 0, 0, name.c_str()) != 0.0f));
                           x.out(1, Value::ofBool(x.api().input(x.ctx(), 1, 0, name.c_str()) != 0.0f));
                       }));
    lib.push_back(pure("input.axis", "Input Axis", "Input", "An input axis (-1..1), e.g. MoveX.", {str("axis", "MoveX")},
                       {num("value")}, [](Exec& x) {
                           const std::string name = x.text(0);
                           x.out(0, Value::ofNumber(x.api().input(x.ctx(), 2, 0, name.c_str())));
                       }));
    lib.push_back(pure("input.moveVector", "Move Input", "Input", "MoveX / MoveY axes as a flat vector (x, 0, -y).", {},
                       {vec("vector")}, [](Exec& x) {
                           const float mx = x.api().input(x.ctx(), 2, 0, "MoveX");
                           const float my = x.api().input(x.ctx(), 2, 0, "MoveY");
                           x.out(0, Value::ofVec3({mx, 0.0f, -my}));
                       }));
    lib.push_back(pure("input.mouseDown", "Mouse Button Down", "Input", "A mouse button is held.",
                       {str("button", "Left", PinHint::MouseButton)}, {boolean("down"), boolean("pressed")}, [](Exec& x) {
                           const int b = std::max(0, script_names::indexOf(script_names::kMouseButtons, x.text(0)));
                           x.out(0, Value::ofBool(x.api().mouseDown(x.ctx(), b) != 0));
                           x.out(1, Value::ofBool(x.api().inputEdge(x.ctx(), 0, b) != 0));
                       }));
    lib.push_back(pure("input.mouse", "Mouse", "Input", "Mouse position (HUD pixels) and movement this frame.", {},
                       {vec("position"), vec("delta")}, [](Exec& x) {
                           float p[2] = {0, 0};
                           float d[2] = {0, 0};
                           x.api().mousePosition(x.ctx(), p);
                           x.api().mouseDelta(x.ctx(), d);
                           x.out(0, Value::ofVec3({p[0], p[1], 0}));
                           x.out(1, Value::ofVec3({d[0], d[1], 0}));
                       }));
    lib.push_back(pure("input.gamepadButton", "Gamepad Button", "Input", "A gamepad button is held.",
                       {str("button", "A", PinHint::GamepadButton)}, {boolean("down"), boolean("pressed")}, [](Exec& x) {
                           const int b = std::max(0, script_names::indexOf(script_names::kGamepadButtons, x.text(0)));
                           x.out(0, Value::ofBool(x.api().input(x.ctx(), 4, b, nullptr) != 0.0f));
                           x.out(1, Value::ofBool(x.api().input(x.ctx(), 5, b, nullptr) != 0.0f));
                       }));
    lib.push_back(pure("input.gamepadAxis", "Gamepad Axis", "Input", "A stick (-1..1) or trigger (0..1).",
                       {str("axis", "LeftX", PinHint::GamepadAxis)}, {num("value")}, [](Exec& x) {
                           const int a = std::max(0, script_names::indexOf(script_names::kGamepadAxes, x.text(0)));
                           x.out(0, Value::ofNumber(x.api().input(x.ctx(), 6, a, nullptr)));
                       }));

    // -- Time
    lib.push_back(pure("time.now", "Time", "Time", "Simulated seconds since Play.", {}, {num("seconds")},
                       [](Exec& x) { x.out(0, Value::ofNumber(x.api().time(x.ctx()))); }));
    lib.push_back(pure("time.delta", "Delta Time", "Time", "Seconds of the current step.", {}, {num("dt")},
                       [](Exec& x) { x.out(0, Value::ofNumber(x.instance().deltaTime())); }));

    // -- Audio, animation, particles, rendering
    lib.push_back(action("audio.play", "Play Sound", "Audio", "Plays the entity's Audio Source.", {ent()}, {},
                         [](Exec& x) { x.api().sound(x.ctx(), x.entity(1), 1); }));
    lib.push_back(action("audio.stop", "Stop Sound", "Audio", "Stops the entity's Audio Source.", {ent()}, {},
                         [](Exec& x) { x.api().sound(x.ctx(), x.entity(1), 0); }));
    lib.push_back(pure("audio.playing", "Is Sound Playing", "Audio", "The Audio Source is playing.", {ent()},
                       {boolean("playing")}, [](Exec& x) { x.out(0, Value::ofBool(x.api().sound(x.ctx(), x.entity(0), 2) != 0)); }));
    lib.push_back(action("anim.play", "Play Animation", "Animation", "Plays a clip of the Animator (empty: its default).",
                         {ent(), str("clip")}, {}, [](Exec& x) {
                             const std::string clip = x.text(2);
                             x.api().animation(x.ctx(), x.entity(1), clip.c_str(), 1);
                         }));
    lib.push_back(action("anim.crossfade", "Crossfade Animation", "Animation", "Blends into a clip over some seconds.",
                         {ent(), str("clip"), num("seconds", 0.25)}, {}, [](Exec& x) {
                             const std::string clip = x.text(2);
                             x.api().animationBlend(x.ctx(), x.entity(1), clip.c_str(), static_cast<float>(x.number(3)));
                         }));
    lib.push_back(action("anim.stop", "Stop Animation", "Animation", "Stops the Animator.", {ent()}, {},
                         [](Exec& x) { x.api().animation(x.ctx(), x.entity(1), "", 0); }));
    lib.push_back(action("particles.burst", "Emit Particles", "Effects", "Emits a burst of particles.", {ent(), num("count", 20.0)},
                         {}, [](Exec& x) { x.api().particles(x.ctx(), x.entity(1), 2, static_cast<int>(x.number(2))); }));
    lib.push_back(action("particles.playing", "Set Particles Playing", "Effects", "Starts or stops an emitter.",
                         {ent(), boolean("playing", true)}, {},
                         [](Exec& x) { x.api().particles(x.ctx(), x.entity(1), x.flag(2) ? 1 : 0, 0); }));
    lib.push_back(action("render.setColor", "Set Color", "Rendering", "Sets the Mesh Renderer's base colour.",
                         {ent(), vec("color", "1,1,1")}, {}, [](Exec& x) {
                             const Vec3 c = x.vec(2);
                             const float rgb[3] = {c.x, c.y, c.z};
                             x.api().setColor(x.ctx(), x.entity(1), rgb);
                         }));
    lib.push_back(action("render.setMaterial", "Set Material", "Rendering", "Switches to a .lmat material (empty: own).",
                         {ent(), str("material", "Content/Materials/", PinHint::Asset)}, {}, [](Exec& x) {
                             const std::string path = x.text(2);
                             x.api().setMaterial(x.ctx(), x.entity(1), path.c_str());
                         }));

    // -- HUD and UI
    lib.push_back(action("hud.text", "Draw Text", "HUD", "Draws text over the game this step.",
                         {any("text"), num("x", 20.0), num("y", 20.0), num("size", 24.0), vec("color", "1,1,1"),
                          num("alpha", 1.0)},
                         {}, [](Exec& x) {
                             const std::string text = x.in(1).asString();
                             const float xys[3] = {static_cast<float>(x.number(2)), static_cast<float>(x.number(3)),
                                                   static_cast<float>(x.number(4))};
                             const Vec3 c = x.vec(5);
                             const float rgba[4] = {c.x, c.y, c.z, static_cast<float>(x.number(6))};
                             x.api().drawText(x.ctx(), text.c_str(), xys, rgba);
                         }));
    lib.push_back(action("hud.rect", "Draw Rectangle", "HUD", "Draws a filled rectangle over the game this step.",
                         {num("x", 20.0), num("y", 20.0), num("width", 100.0), num("height", 20.0), vec("color", "1,1,1"),
                          num("alpha", 1.0)},
                         {}, [](Exec& x) {
                             const float r[4] = {static_cast<float>(x.number(1)), static_cast<float>(x.number(2)),
                                                 static_cast<float>(x.number(3)), static_cast<float>(x.number(4))};
                             const Vec3 c = x.vec(5);
                             const float rgba[4] = {c.x, c.y, c.z, static_cast<float>(x.number(6))};
                             x.api().drawRect(x.ctx(), r, rgba);
                         }));
    lib.push_back(pure("hud.screen", "Screen Size", "HUD", "HUD width and height in pixels.", {}, {num("width"), num("height")},
                       [](Exec& x) {
                           float wh[2] = {0, 0};
                           x.api().screenSize(x.ctx(), wh);
                           x.out(0, Value::ofNumber(wh[0]));
                           x.out(1, Value::ofNumber(wh[1]));
                       }));
    lib.push_back(flow("ui.button", "UI Button", "Draws a button; runs Clicked when it is clicked.",
                       {str("text", "Play"), num("x", 20.0), num("y", 20.0), num("width", 220.0), num("height", 48.0)},
                       {execPin("then"), execPin("clicked")}, [](Exec& x) {
                           const std::string text = x.text(1);
                           const float rect[4] = {static_cast<float>(x.number(2)), static_cast<float>(x.number(3)),
                                                  static_cast<float>(x.number(4)), static_cast<float>(x.number(5))};
                           const bool clicked = x.api().ui(x.ctx(), 0, text.c_str(), rect, nullptr, nullptr) != 0;
                           x.fire(0);
                           if (clicked) x.fire(1);
                       }));

    // -- Game
    lib.push_back(action("scene.load", "Load Scene", "Game", "Switches to another scene at the end of the step.",
                         {str("scene", "Scenes/", PinHint::Asset)}, {}, [](Exec& x) {
                             const std::string path = x.text(1);
                             x.api().loadScene(x.ctx(), path.c_str());
                         }));
    lib.push_back(action("game.menu", "Open Game Menu", "Game", "Opens the built-in pause menu.", {}, {},
                         [](Exec& x) { x.api().gameMenu(x.ctx(), 0); }));
    lib.push_back(action("save.setNumber", "Save Number", "Game", "Stores a number in the save file.",
                         {str("key", "score"), num("value")}, {}, [](Exec& x) {
                             const std::string key = x.text(1);
                             x.api().save(x.ctx(), 0, key.c_str(), nullptr, x.number(2), nullptr, 0, nullptr);
                         }));
    lib.push_back(pure("save.getNumber", "Load Number", "Game", "Reads a number from the save file.",
                       {str("key", "score"), num("fallback")}, {num("value")}, [](Exec& x) {
                           const std::string key = x.text(0);
                           double out = x.number(1);
                           if (x.api().save(x.ctx(), 1, key.c_str(), nullptr, 0.0, nullptr, 0, &out) == 0) out = x.number(1);
                           x.out(0, Value::ofNumber(out));
                       }));
    lib.push_back(action("nav.moveTo", "Move To (Navigation)", "AI", "Walks the Nav Agent to a point.", {vec("target")}, {},
                         [](Exec& x) {
                             const Vec3 t = x.vec(1);
                             const float p[3] = {t.x, t.y, t.z};
                             x.api().navigation(x.ctx(), x.self(), 1, p, nullptr, nullptr, 0);
                         }));
    lib.push_back(action("nav.stop", "Stop Moving", "AI", "Stops the Nav Agent.", {}, {},
                         [](Exec& x) { x.api().navigation(x.ctx(), x.self(), 2, nullptr, nullptr, nullptr, 0); }));
    lib.push_back(pure("nav.moving", "Is Moving", "AI", "The Nav Agent is walking a path.", {}, {boolean("moving")},
                       [](Exec& x) {
                           x.out(0, Value::ofBool(x.api().navigation(x.ctx(), x.self(), 3, nullptr, nullptr, nullptr, 0) != 0));
                       }));
    lib.push_back(action("timeline.play", "Play Timeline", "Game", "Plays the entity's Timeline from the start.", {ent()}, {},
                         [](Exec& x) { x.api().timeline(x.ctx(), x.entity(1), 0, nullptr, nullptr); }));
    lib.push_back(action("vehicle.drive", "Drive Vehicle", "Physics", "Throttle, steering (-1..1), brake, handbrake.",
                         {num("throttle"), num("steering"), num("brake"), num("handbrake")}, {}, [](Exec& x) {
                             const float in[4] = {static_cast<float>(x.number(1)), static_cast<float>(x.number(2)),
                                                  static_cast<float>(x.number(3)), static_cast<float>(x.number(4))};
                             x.api().vehicle(x.ctx(), x.self(), 0, in, nullptr);
                         }));

    // -- Debug
    lib.push_back(action("debug.log", "Print", "Debug", "Writes a message to the console.", {any("message")}, {}, [](Exec& x) {
        const std::string text = x.in(1).asString();
        x.api().log(x.ctx(), 0, text.c_str());
    }));
    lib.push_back(action("debug.line", "Debug Line", "Debug", "Draws a line for one step.",
                         {vec("from"), vec("to", "0,1,0"), vec("color", "1,0,0")}, {}, [](Exec& x) {
                             const Vec3 a = x.vec(1), b = x.vec(2), c = x.vec(3);
                             const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z}, rgba[4] = {c.x, c.y, c.z, 1.0f};
                             x.api().debugDraw(x.ctx(), 0, pa, pb, rgba);
                         }));
    lib.push_back(action("debug.sphere", "Debug Sphere", "Debug", "Draws a wire sphere for one step.",
                         {vec("center"), num("radius", 0.5), vec("color", "0,1,0")}, {}, [](Exec& x) {
                             const Vec3 a = x.vec(1), c = x.vec(3);
                             const float r = static_cast<float>(x.number(2));
                             const float pa[3] = {a.x, a.y, a.z}, pb[3] = {r, r, r}, rgba[4] = {c.x, c.y, c.z, 1.0f};
                             x.api().debugDraw(x.ctx(), 2, pa, pb, rgba);
                         }));
    return lib;
}

Value literalDefault(const PinDef& pin) {
    switch (pin.type) {
    case PinType::Number: return Value::ofNumber(pin.defaultNumber);
    case PinType::Bool: return Value::ofBool(pin.defaultNumber != 0.0);
    case PinType::Vec3: return Value::ofVec3(Value::ofString(std::string(pin.defaultText)).asVec3());
    case PinType::String: return Value::ofString(std::string(pin.defaultText));
    case PinType::Entity: return Value::ofEntity(kNoEntity);
    default: return Value::ofNumber(pin.defaultNumber);
    }
}

} // namespace

const std::vector<NodeDef>& nodeLibrary() {
    static const std::vector<NodeDef> library = buildLibrary();
    return library;
}

const NodeDef* findNode(std::string_view type) {
    for (const NodeDef& d : nodeLibrary()) {
        if (d.type == type) return &d;
    }
    return nullptr;
}

// ---- Graph ------------------------------------------------------------------------------------

Result<std::shared_ptr<const Graph>> Graph::parse(const nlohmann::json& doc, std::vector<std::string>* warnings) {
    auto warn = [&](std::string message) {
        if (warnings != nullptr) warnings->push_back(std::move(message));
    };
    if (!doc.is_object()) return makeError("a visual script must be a JSON object");
    auto graph = std::make_shared<Graph>();

    if (const auto it = doc.find("variables"); it != doc.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (!v.is_object() || !v.contains("name") || !v["name"].is_string()) continue;
            GraphVariable var;
            var.name = v["name"].get<std::string>();
            var.type = pinTypeFromName(v.value("type", std::string("number")));
            var.initial = Value::fromJson(v.contains("value") ? v["value"] : nlohmann::json(), var.type);
            graph->variables_.push_back(std::move(var));
        }
    }

    std::unordered_map<int, int> indexOfId;
    if (const auto it = doc.find("nodes"); it != doc.end() && it->is_array()) {
        for (const auto& n : *it) {
            if (!n.is_object()) continue;
            const std::string type = n.value("type", std::string());
            const NodeDef* def = findNode(type);
            const int id = n.value("id", 0);
            if (def == nullptr) {
                warn(std::format("node {}: unknown type '{}'", id, type));
                continue;
            }
            if (indexOfId.contains(id)) {
                warn(std::format("node {}: duplicate id", id));
                continue;
            }
            Node node;
            node.id = id;
            node.def = def;
            node.sources.assign(def->inputs.size(), {-1, -1});
            node.execTargets.resize(def->outputs.size());
            const nlohmann::json* inputs = n.contains("inputs") && n["inputs"].is_object() ? &n["inputs"] : nullptr;
            for (const PinDef& pin : def->inputs) {
                if (inputs != nullptr && inputs->contains(pin.name)) {
                    node.literals.push_back(Value::fromJson((*inputs)[std::string(pin.name)], pin.type));
                } else {
                    node.literals.push_back(literalDefault(pin));
                }
            }
            indexOfId[id] = static_cast<int>(graph->nodes_.size());
            graph->nodes_.push_back(std::move(node));
        }
    }

    if (const auto it = doc.find("links"); it != doc.end() && it->is_array()) {
        for (const auto& l : *it) {
            if (!l.is_object()) continue;
            const auto from = indexOfId.find(l.value("from", -1));
            const auto to = indexOfId.find(l.value("to", -1));
            if (from == indexOfId.end() || to == indexOfId.end()) {
                warn("a link points to a missing node");
                continue;
            }
            Node& src = graph->nodes_[static_cast<std::size_t>(from->second)];
            Node& dst = graph->nodes_[static_cast<std::size_t>(to->second)];
            const int out = src.def->outputIndex(l.value("fromPin", std::string()));
            const int in = dst.def->inputIndex(l.value("toPin", std::string()));
            if (out < 0 || in < 0) {
                warn(std::format("link {} -> {}: unknown pin", src.id, dst.id));
                continue;
            }
            const bool srcExec = src.def->outputs[static_cast<std::size_t>(out)].type == PinType::Exec;
            const bool dstExec = dst.def->inputs[static_cast<std::size_t>(in)].type == PinType::Exec;
            if (srcExec != dstExec) {
                warn(std::format("link {} -> {}: exec and data pins cannot be joined", src.id, dst.id));
                continue;
            }
            if (srcExec) {
                src.execTargets[static_cast<std::size_t>(out)].push_back(to->second);
            } else {
                dst.sources[static_cast<std::size_t>(in)] = {from->second, out};
            }
        }
    }
    return std::shared_ptr<const Graph>(std::move(graph));
}

Result<std::shared_ptr<const Graph>> Graph::load(const std::filesystem::path& file, std::vector<std::string>* warnings) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return makeError("cannot open {}", file.string());
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded()) return makeError("{} is not valid JSON", file.string());
    return parse(doc, warnings);
}

std::vector<int> Graph::events(std::string_view type) const {
    std::vector<int> out;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].def->event && nodes_[i].def->type == type) out.push_back(static_cast<int>(i));
    }
    return out;
}

// ---- GraphInstance -----------------------------------------------------------------------------

namespace {
constexpr std::size_t kStepBudget = 250000;  // node runs per event: stops endless loops
constexpr int kMaxDepth = 256;
} // namespace

GraphInstance::GraphInstance(std::shared_ptr<const Graph> graph, const script::EngineApi* api, Entity self)
    : graph_(std::move(graph)), api_(api), self_(self) {
    for (const GraphVariable& v : graph_->variables()) variables_[v.name] = v.initial;
    outputs_.resize(graph_->nodes().size());
    for (std::size_t i = 0; i < graph_->nodes().size(); ++i) {
        const NodeDef& def = *graph_->nodes()[i].def;
        outputs_[i].reserve(def.outputs.size());
        for (const PinDef& pin : def.outputs) {
            outputs_[i].push_back(pin.type == PinType::Bool ? Value::ofBool(false)
                                  : pin.type == PinType::Vec3 ? Value::ofVec3({})
                                  : pin.type == PinType::String ? Value::ofString({})
                                  : pin.type == PinType::Entity ? Value::ofEntity(kNoEntity)
                                                                : Value::ofNumber(0.0));
        }
    }
    state_.assign(graph_->nodes().size(), 0.0);
}

void GraphInstance::fail(std::string message) {
    if (!failed_) error_ = std::move(message);
    failed_ = true;
}

void GraphInstance::runNode(int node) {
    if (failed_) return;
    if (++budget_ > kStepBudget) {
        fail("too many nodes ran in one step (an endless loop?)");
        return;
    }
    if (depth_ >= kMaxDepth) {
        fail("execution nested too deeply (a node triggers itself?)");
        return;
    }
    ++depth_;
    Exec x(*this, node);
    const NodeDef& def = *graph_->nodes()[static_cast<std::size_t>(node)].def;
    try {
        def.run(x);
    } catch (const std::exception& e) {
        fail(std::format("{}: {}", def.title, e.what()));
    }
    --depth_;
}

void GraphInstance::runExec(int node, int outputPin) {
    const auto& targets = graph_->nodes()[static_cast<std::size_t>(node)].execTargets;
    if (outputPin < 0 || static_cast<std::size_t>(outputPin) >= targets.size()) return;
    for (int target : targets[static_cast<std::size_t>(outputPin)]) {
        if (failed_) return;
        runNode(target);
    }
}

void GraphInstance::schedule(int node, int outputPin, float seconds) {
    if (pending_.size() >= 4096) {
        fail("too many pending delays");
        return;
    }
    pending_.push_back({std::max(0.0f, seconds), node, outputPin});
}

Value GraphInstance::evaluate(int node, int inputPin) {
    const Graph::Node& n = graph_->nodes()[static_cast<std::size_t>(node)];
    if (inputPin < 0 || static_cast<std::size_t>(inputPin) >= n.sources.size()) return {};
    const auto [source, pin] = n.sources[static_cast<std::size_t>(inputPin)];
    if (source < 0) return n.literals[static_cast<std::size_t>(inputPin)];
    if (graph_->nodes()[static_cast<std::size_t>(source)].def->pure) runNode(source);  // recomputed on each read
    return outputs_[static_cast<std::size_t>(source)][static_cast<std::size_t>(pin)];
}

bool GraphInstance::fire(std::string_view eventType, const std::function<bool(int)>& prepare) {
    for (int node : graph_->events(eventType)) {
        if (failed_) break;
        budget_ = 0;
        if (prepare && !prepare(node)) continue;
        runExec(node, 0);
    }
    return !failed_;
}

bool GraphInstance::start() {
    if (failed_) return false;
    return fire("event.start", nullptr);
}

bool GraphInstance::update(float dt) {
    if (failed_) return false;
    dt_ = dt;
    // Delays that are due (they may schedule new ones while running).
    if (!pending_.empty()) {
        std::vector<Pending> due;
        for (auto it = pending_.begin(); it != pending_.end();) {
            it->remaining -= dt;
            if (it->remaining <= 0.0f) {
                due.push_back(*it);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        for (const Pending& p : due) {
            budget_ = 0;
            runExec(p.node, p.pin);
            if (failed_) return false;
        }
    }
    fire("event.update", [&](int node) {
        output(node, 1) = Value::ofNumber(dt);
        return true;
    });
    fire("event.every", [&](int node) {
        const double interval = std::max(1e-3, evaluate(node, 0).asNumber());
        double& elapsed = nodeState(node);
        elapsed += dt;
        if (elapsed < interval) return false;
        elapsed = std::fmod(elapsed, interval);
        return true;
    });
    fire("event.key", [&](int node) {
        const int key = script_names::indexOf(script_names::kKeys, evaluate(node, 0).asString());
        return key >= 0 && api_->inputEdge(api_->context, 2, key) != 0;
    });
    fire("event.action", [&](int node) {
        const std::string name = evaluate(node, 0).asString();
        return api_->input(api_->context, 1, 0, name.c_str()) != 0.0f;
    });
    fire("event.timeline", [&](int node) {
        const std::string name = evaluate(node, 0).asString();
        return !name.empty() && api_->timeline(api_->context, script::kNoEntity, 4, name.c_str(), nullptr) != 0;
    });
    return !failed_;
}

bool GraphInstance::collision(Entity other) {
    if (failed_) return false;
    return fire("event.collision", [&](int node) {
        output(node, 1) = Value::ofEntity(other);
        return true;
    });
}

Entity Exec::entity(int pin) const {
    const Graph::Node& n = instance_.graph_->nodes()[static_cast<std::size_t>(node_)];
    const Value v = in(pin);
    const bool linked = static_cast<std::size_t>(pin) < n.sources.size() && n.sources[static_cast<std::size_t>(pin)].first >= 0;
    if (linked) return v.type == PinType::Entity ? v.entity : kNoEntity;
    if (v.type == PinType::Entity && v.entity != kNoEntity) return v.entity;
    if (!v.text.empty()) return instance_.api().find(instance_.api().context, v.text.c_str());
    return instance_.self();
}

} // namespace lumora::visual
