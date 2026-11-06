// Visual scripting: node graphs (.lgraph) that run as behaviours, without writing code.
//
// A graph is a set of nodes joined by links. Event nodes (On Start, On Update, On Collision,
// On Key Pressed, ...) start execution; white "exec" links decide the order in which action
// nodes run; coloured data links feed values (numbers, booleans, vectors, text, entities) into
// node inputs. Pure nodes (maths, comparisons, getters) have no exec pins and are evaluated
// whenever an input that needs them is read. An input with no link uses the literal value saved
// in the node (the editor shows a field for it); an Entity input left empty means "this entity".
//
// Graphs talk to the engine through the same C function table as C++ and Lua behaviours
// (lumora::script::EngineApi), so all three can do the same things. The node library below is
// shared by the runtime and the editor's graph panel (one definition of every node).
//
// File format (JSON):
//   { "version": 1,
//     "variables": [ { "name": "speed", "type": "number", "value": 4 } ],
//     "nodes": [ { "id": 1, "type": "event.update", "x": 0, "y": 0, "inputs": { "seconds": 1 } } ],
//     "links": [ { "from": 1, "fromPin": "exec", "to": 2, "toPin": "in" } ] }
#pragma once

#include "core/Error.hpp"

#include <lumora/Script.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lumora::visual {

enum class PinType : std::uint8_t { Exec, Number, Bool, Vec3, String, Entity, Any };

[[nodiscard]] std::string_view pinTypeName(PinType type) noexcept;
/// "number", "bool", "vec3", "string", "entity" (variables); Any for anything else.
[[nodiscard]] PinType pinTypeFromName(std::string_view name) noexcept;

/// A value flowing through a data link. Conversions are lenient: numbers and booleans convert
/// into each other, a number becomes a vector (n, n, n), anything prints as text.
struct Value {
    PinType type = PinType::Number;
    double number = 0.0;
    script::Vec3 vec;
    std::string text;
    script::Entity entity = script::kNoEntity;

    static Value ofNumber(double n) { Value v; v.type = PinType::Number; v.number = n; return v; }
    static Value ofBool(bool b) { Value v; v.type = PinType::Bool; v.number = b ? 1.0 : 0.0; return v; }
    static Value ofVec3(script::Vec3 x) { Value v; v.type = PinType::Vec3; v.vec = x; return v; }
    static Value ofString(std::string s) { Value v; v.type = PinType::String; v.text = std::move(s); return v; }
    static Value ofEntity(script::Entity e) { Value v; v.type = PinType::Entity; v.entity = e; return v; }

    [[nodiscard]] double asNumber() const;
    [[nodiscard]] bool asBool() const;
    [[nodiscard]] script::Vec3 asVec3() const;
    [[nodiscard]] std::string asString() const;

    /// A literal saved in a graph file (null gives the type's zero value).
    static Value fromJson(const nlohmann::json& j, PinType type);
    [[nodiscard]] nlohmann::json toJson() const;
};

/// Editor hint for a String input: offer a fixed list instead of free text.
enum class PinHint : std::uint8_t { None, Key, MouseButton, GamepadButton, GamepadAxis, Variable, Asset };

struct PinDef {
    std::string_view name;
    PinType type = PinType::Number;
    std::string_view label;     // shown in the editor; empty = name
    double defaultNumber = 0.0;  // Number / Bool literal default
    std::string_view defaultText; // String default; Vec3 default as "x,y,z"
    PinHint hint = PinHint::None;
};

class Exec;

struct NodeDef {
    std::string_view type;      // "math.add"
    std::string_view title;     // "Add"
    std::string_view category;  // "Math"
    std::string_view description;
    std::vector<PinDef> inputs;   // exec inputs first (impure nodes have one named "in")
    std::vector<PinDef> outputs;  // exec outputs first
    bool event = false;  // starts execution; no exec input
    bool pure = false;   // no exec pins; evaluated on demand
    std::function<void(Exec&)> run;

    [[nodiscard]] int inputIndex(std::string_view pin) const noexcept;
    [[nodiscard]] int outputIndex(std::string_view pin) const noexcept;
};

/// Every node type, in menu order.
[[nodiscard]] const std::vector<NodeDef>& nodeLibrary();
[[nodiscard]] const NodeDef* findNode(std::string_view type);

struct GraphVariable {
    std::string name;
    PinType type = PinType::Number;
    Value initial;
};

/// A parsed graph, shared by every instance that runs it.
class Graph {
public:
    struct Node {
        int id = 0;
        const NodeDef* def = nullptr;
        std::vector<Value> literals;  // per input pin
        // Per input pin: the (node index, output pin) feeding it, or -1.
        std::vector<std::pair<int, int>> sources;
        // Per output pin: for exec pins, the node indices it runs.
        std::vector<std::vector<int>> execTargets;
    };

    /// Unknown node types and dangling links are skipped with a message in `warnings`.
    [[nodiscard]] static Result<std::shared_ptr<const Graph>> parse(const nlohmann::json& document,
                                                                   std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static Result<std::shared_ptr<const Graph>> load(const std::filesystem::path& file,
                                                                  std::vector<std::string>* warnings = nullptr);

    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<GraphVariable>& variables() const noexcept { return variables_; }
    /// Node indices of every event node of this type.
    [[nodiscard]] std::vector<int> events(std::string_view type) const;

private:
    std::vector<Node> nodes_;
    std::vector<GraphVariable> variables_;
};

/// One running copy of a graph, attached to an entity. Methods return false after a runtime
/// error (error() says what); the behaviour is then disabled by its runner.
class GraphInstance {
public:
    GraphInstance(std::shared_ptr<const Graph> graph, const script::EngineApi* api, script::Entity self);

    bool start();
    bool update(float dt);
    bool collision(script::Entity other);
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Used by node implementations (through Exec).
    [[nodiscard]] const script::EngineApi& api() const noexcept { return *api_; }
    [[nodiscard]] script::Entity self() const noexcept { return self_; }
    [[nodiscard]] float deltaTime() const noexcept { return dt_; }
    Value& variable(const std::string& name) { return variables_[name]; }
    double& nodeState(int node) { return state_[static_cast<std::size_t>(node)]; }
    void runExec(int node, int outputPin);  // follows the links of an exec output
    void schedule(int node, int outputPin, float seconds);
    [[nodiscard]] Value evaluate(int node, int inputPin);
    [[nodiscard]] Value& output(int node, int pin) { return outputs_[static_cast<std::size_t>(node)][static_cast<std::size_t>(pin)]; }
    void fail(std::string message);

private:
    friend class Exec;
    void runNode(int node);
    bool fire(std::string_view eventType, const std::function<bool(int node)>& prepare);

    std::shared_ptr<const Graph> graph_;
    const script::EngineApi* api_;
    script::Entity self_;
    std::unordered_map<std::string, Value> variables_;
    std::vector<std::vector<Value>> outputs_;  // last values written by each node
    std::vector<double> state_;                // per-node scratch (Do Once, Flip Flop, timers)
    struct Pending {
        float remaining;
        int node;
        int pin;
    };
    std::vector<Pending> pending_;
    std::string error_;
    float dt_ = 0.0f;
    int depth_ = 0;
    std::size_t budget_ = 0;
    bool failed_ = false;
};

/// What a node implementation sees while it runs.
class Exec {
public:
    Exec(GraphInstance& instance, int node) : instance_(instance), node_(node) {}

    [[nodiscard]] Value in(int pin) const { return instance_.evaluate(node_, pin); }
    [[nodiscard]] double number(int pin) const { return in(pin).asNumber(); }
    [[nodiscard]] bool flag(int pin) const { return in(pin).asBool(); }
    [[nodiscard]] script::Vec3 vec(int pin) const { return in(pin).asVec3(); }
    [[nodiscard]] std::string text(int pin) const { return in(pin).asString(); }
    /// An Entity input: linked value, else the literal name looked up in the scene, else self.
    [[nodiscard]] script::Entity entity(int pin) const;

    void out(int pin, Value value) const { instance_.output(node_, pin) = std::move(value); }
    /// Runs what the exec output `pin` is linked to, now.
    void fire(int pin) const { instance_.runExec(node_, pin); }
    void fireLater(int pin, float seconds) const { instance_.schedule(node_, pin, seconds); }

    [[nodiscard]] const script::EngineApi& api() const { return instance_.api(); }
    [[nodiscard]] void* ctx() const { return instance_.api().context; }
    [[nodiscard]] script::Entity self() const { return instance_.self(); }
    [[nodiscard]] GraphInstance& instance() const { return instance_; }
    [[nodiscard]] int node() const { return node_; }
    double& state() const { return instance_.nodeState(node_); }

private:
    GraphInstance& instance_;
    int node_;
};

} // namespace lumora::visual
