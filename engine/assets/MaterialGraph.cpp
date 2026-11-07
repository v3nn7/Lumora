#include "assets/MaterialGraph.hpp"

#include "core/FileSystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>

namespace lumora {

namespace matgraph {

namespace {

Pin flt(std::string_view name, float value, std::string_view label = {}) { return {name, PinKind::Float, label, value}; }
Pin vec(std::string_view name, float x, float y, float z, std::string_view label = {}) { return {name, PinKind::Vector, label, x, y, z}; }
Pin out(std::string_view name, PinKind kind, std::string_view label = {}) { return {name, kind, label}; }
/// Link-only inputs that default to a property of the surface (UV, position) when unlinked.
Pin automatic(std::string_view name, std::string_view label) { return {name, PinKind::Vector, label, 0, 0, 0, "auto"}; }
Pin texture(std::string_view name) { return {name, PinKind::Texture, "Texture"}; }

std::vector<NodeType> makeLibrary() {
    std::vector<NodeType> lib;
    auto add = [&](std::string_view type, std::string_view title, std::string_view category, std::string_view description,
                   std::vector<Pin> inputs, std::vector<Pin> outputs) {
        lib.push_back({type, title, category, description, std::move(inputs), std::move(outputs)});
    };
    // Output
    add(kOutputNode, "Material Output", "Output",
        "The surface: colour, opacity, metallic, roughness, a tangent-space normal, emission, occlusion and a world-space "
        "offset per vertex (wind). Alpha mode: 0 opaque, 1 cut out (below the cutoff), 2 transparent.",
        {vec("baseColor", 0.8f, 0.8f, 0.8f, "Base Color"), flt("opacity", 1.0f, "Opacity"), flt("metallic", 0.0f, "Metallic"),
         flt("roughness", 0.5f, "Roughness"), vec("normal", 0.0f, 0.0f, 1.0f, "Normal"), vec("emissive", 0, 0, 0, "Emissive"),
         flt("occlusion", 1.0f, "Occlusion"), vec("worldOffset", 0, 0, 0, "World Offset"), flt("alphaMode", 0.0f, "Alpha Mode"),
         flt("alphaCutoff", 0.5f, "Alpha Cutoff"), {"twoSided", PinKind::Bool, "Two Sided"}},
        {});
    // Inputs
    add("input.uv", "UV", "Input", "The first texture coordinate set.", {}, {out("uv", PinKind::Vector)});
    add("input.uv2", "UV 2", "Input", "The second texture coordinate set (glTF TEXCOORD_1).", {}, {out("uv", PinKind::Vector)});
    add("input.worldPosition", "World Position", "Input", "Where the surface is, in metres.", {},
        {out("position", PinKind::Vector)});
    add("input.objectPosition", "Object Position", "Input", "Where the vertex is in the mesh's own space (wind masks).", {},
        {out("position", PinKind::Vector)});
    add("input.normal", "World Normal", "Input", "The surface's direction (before normal maps).", {}, {out("normal", PinKind::Vector)});
    add("input.view", "View Direction", "Input", "From the surface towards the camera.", {}, {out("view", PinKind::Vector)});
    add("input.time", "Time", "Input", "Seconds since the start.", {}, {out("time", PinKind::Float)});
    // Constants
    add("const.number", "Number", "Constant", "A number.", {flt("value", 1.0f, "Value")}, {out("value", PinKind::Float)});
    add("const.color", "Color", "Constant", "A colour (or any three numbers).", {vec("color", 1, 1, 1, "Color")},
        {out("color", PinKind::Vector)});
    // Textures
    add("texture.color", "Texture", "Texture", "Samples a colour map (sRGB): its colour and alpha.",
        {texture("texture"), automatic("uv", "UV")}, {out("rgb", PinKind::Vector, "RGB"), out("a", PinKind::Float, "A")});
    add("texture.data", "Data Texture", "Texture", "Samples a map of values (masks, roughness, metallic): each channel.",
        {texture("texture"), automatic("uv", "UV")},
        {out("rgb", PinKind::Vector, "RGB"), out("r", PinKind::Float, "R"), out("g", PinKind::Float, "G"),
         out("b", PinKind::Float, "B"), out("a", PinKind::Float, "A")});
    add("texture.normal", "Normal Map", "Texture", "Samples a tangent-space normal map (for the Normal output).",
        {texture("texture"), automatic("uv", "UV"), flt("strength", 1.0f, "Strength")}, {out("normal", PinKind::Vector)});
    // UV
    add("uv.tiling", "Tiling & Offset", "UV", "Repeats and shifts texture coordinates.",
        {automatic("uv", "UV"), vec("tiling", 1, 1, 0, "Tiling"), vec("offset", 0, 0, 0, "Offset")}, {out("uv", PinKind::Vector)});
    add("uv.panner", "Panner", "UV", "Slides texture coordinates over time (flowing water, conveyor belts).",
        {automatic("uv", "UV"), vec("speed", 0.1f, 0, 0, "Speed")}, {out("uv", PinKind::Vector)});
    // Math
    auto binary = [&](std::string_view type, std::string_view title, std::string_view description, float b) {
        add(type, title, "Math", description, {vec("a", 0, 0, 0, "A"), vec("b", b, b, b, "B")}, {out("result", PinKind::Vector)});
    };
    binary("math.add", "Add", "A + B, per component.", 0.0f);
    binary("math.subtract", "Subtract", "A - B, per component.", 0.0f);
    binary("math.multiply", "Multiply", "A x B, per component (tint a texture, scale a mask).", 1.0f);
    binary("math.divide", "Divide", "A / B, per component.", 1.0f);
    binary("math.min", "Min", "The smaller of A and B, per component.", 0.0f);
    binary("math.max", "Max", "The larger of A and B, per component.", 0.0f);
    binary("math.power", "Power", "A to the power B, per component.", 1.0f);
    add("math.lerp", "Lerp", "Math", "Blends from A to B by T.", {vec("a", 0, 0, 0, "A"), vec("b", 1, 1, 1, "B"), vec("t", 0.5f, 0.5f, 0.5f, "T")},
        {out("result", PinKind::Vector)});
    add("math.clamp", "Clamp", "Math", "Keeps a value between Min and Max.",
        {vec("value", 0, 0, 0, "Value"), vec("min", 0, 0, 0, "Min"), vec("max", 1, 1, 1, "Max")}, {out("result", PinKind::Vector)});
    add("math.smoothstep", "Smoothstep", "Math", "0 below Edge 0, 1 above Edge 1, smooth between.",
        {vec("edge0", 0, 0, 0, "Edge 0"), vec("edge1", 1, 1, 1, "Edge 1"), vec("value", 0.5f, 0.5f, 0.5f, "Value")},
        {out("result", PinKind::Vector)});
    auto unary = [&](std::string_view type, std::string_view title, std::string_view description) {
        add(type, title, "Math", description, {vec("value", 0, 0, 0, "Value")}, {out("result", PinKind::Vector)});
    };
    unary("math.oneMinus", "One Minus", "1 - value (inverts a mask).");
    unary("math.abs", "Abs", "Absolute value.");
    unary("math.frac", "Fraction", "The part after the decimal point.");
    unary("math.sine", "Sine", "sin(value), value in radians.");
    unary("math.saturate", "Saturate", "Clamps to 0..1.");
    unary("math.normalize", "Normalize", "The direction of a vector (length 1).");
    add("math.length", "Length", "Math", "The length of a vector.", {vec("value", 0, 0, 0, "Value")}, {out("result", PinKind::Float)});
    add("math.dot", "Dot", "Math", "The dot product of two vectors.", {vec("a", 0, 0, 0, "A"), vec("b", 0, 1, 0, "B")},
        {out("result", PinKind::Float)});
    // Vectors
    add("vector.split", "Split", "Vector", "The components of a vector.", {vec("vector", 0, 0, 0, "Vector")},
        {out("x", PinKind::Float, "X"), out("y", PinKind::Float, "Y"), out("z", PinKind::Float, "Z")});
    add("vector.combine", "Combine", "Vector", "A vector from three numbers.", {flt("x", 0, "X"), flt("y", 0, "Y"), flt("z", 0, "Z")},
        {out("vector", PinKind::Vector)});
    // Layers and masks
    add("layer.blend", "Blend Layers", "Layers", "Lays Layer over Base where Mask is 1 (moss on rock, dirt on paint).",
        {vec("base", 0.5f, 0.5f, 0.5f, "Base"), vec("layer", 1, 1, 1, "Layer"), flt("mask", 0.5f, "Mask")},
        {out("result", PinKind::Vector)});
    add("mask.fresnel", "Fresnel", "Layers", "1 at grazing angles, 0 facing the camera (rims, sheen).", {flt("power", 5.0f, "Power")},
        {out("value", PinKind::Float)});
    add("mask.slope", "Slope Mask", "Layers", "1 on surfaces facing up (between Min and Max of the normal's height).",
        {flt("min", 0.6f, "Min"), flt("max", 0.85f, "Max")}, {out("value", PinKind::Float)});
    add("noise.value", "Noise", "Layers", "Smooth random values in space (break up tiling, vary colour).",
        {automatic("position", "Position"), flt("scale", 1.0f, "Scale")}, {out("value", PinKind::Float)});
    // Foliage
    add("wind.sway", "Wind", "Foliage", "Sways vertices in the wind (into World Offset). Mask: how much each part moves, "
        "e.g. the object position's height.",
        {flt("strength", 0.1f, "Strength"), flt("speed", 1.0f, "Speed"), flt("mask", 1.0f, "Mask")}, {out("offset", PinKind::Vector)});
    add("data.reroute", "Reroute", "Flow", "Passes a value straight on.", {vec("value", 0, 0, 0)}, {out("value", PinKind::Vector)});
    return lib;
}

} // namespace

const std::vector<NodeType>& nodeTypes() {
    static const std::vector<NodeType> library = makeLibrary();
    return library;
}

const NodeType* findNodeType(std::string_view type) {
    for (const NodeType& t : nodeTypes()) {
        if (t.type == type) return &t;
    }
    return nullptr;
}

} // namespace matgraph

namespace {

using namespace matgraph;

struct Node {
    const NodeType* type = nullptr;
    nlohmann::json literals;
};

struct Source {
    int node = 0;
    std::string pin;
    auto operator<=>(const Source&) const = default;
};

class Compiler {
public:
    explicit Compiler(const nlohmann::json& doc) : doc_(doc) {}

    Result<MaterialProgram> run() {
        if (!doc_.is_object()) return makeError("a material graph must be a JSON object");
        int output = 0;
        for (const auto& n : doc_.value("nodes", nlohmann::json::array())) {
            if (!n.is_object()) continue;
            const int id = n.value("id", 0);
            const NodeType* type = findNodeType(n.value("type", std::string()));
            if (type == nullptr) return makeError("unknown node type '{}'", n.value("type", std::string()));
            nodes_[id] = {type, n.value("inputs", nlohmann::json::object())};
            if (type->type == kOutputNode) {
                if (output != 0) return makeError("the graph has more than one Material Output");
                output = id;
            }
        }
        if (output == 0) return makeError("the graph has no Material Output");
        for (const auto& l : doc_.value("links", nlohmann::json::array())) {
            const int from = l.value("from", 0), to = l.value("to", 0);
            if (!nodes_.contains(from) || !nodes_.contains(to)) continue;
            links_[{to, l.value("toPin", std::string())}] = {from, l.value("fromPin", std::string())};
        }
        output_ = output;
        const Node& out = nodes_[output];
        program_.alphaMode = std::clamp(static_cast<int>(std::lround(literalFloat(out, "alphaMode", 0.0f))), 0, 2);
        program_.alphaCutoff = std::clamp(literalFloat(out, "alphaCutoff", 0.5f), 0.01f, 1.0f);
        program_.twoSided = literalFloat(out, "twoSided", 0.0f) != 0.0f;

        const std::pair<const char*, std::uint32_t> fragmentOutputs[] = {{"baseColor", kBaseColor}, {"opacity", kOpacity},
                                                                         {"metallic", kMetallic},   {"roughness", kRoughness},
                                                                         {"normal", kNormal},       {"emissive", kEmissive},
                                                                         {"occlusion", kOcclusion}};
        if (auto r = stage(fragmentOutputs, program_.fragment); !r) return std::unexpected(r.error());
        if (links_.contains({output, "worldOffset"})) {  // only graphs that move vertices get a vertex program
            const std::pair<const char*, std::uint32_t> vertexOutputs[] = {{"worldOffset", kWorldOffset}};
            if (auto r = stage(vertexOutputs, program_.vertex); !r) return std::unexpected(r.error());
        }
        return std::move(program_);
    }

private:
    // ---- Literals ----
    static const Pin* inputPin(const Node& n, std::string_view name) {
        for (const Pin& p : n.type->inputs) {
            if (p.name == name) return &p;
        }
        return nullptr;
    }
    static float literalFloat(const Node& n, std::string_view pin, float fallback) {
        const auto it = n.literals.find(std::string(pin));
        if (it == n.literals.end()) {
            const Pin* p = inputPin(n, pin);
            return p != nullptr ? p->x : fallback;
        }
        if (it->is_number()) return it->get<float>();
        if (it->is_boolean()) return it->get<bool>() ? 1.0f : 0.0f;
        return fallback;
    }
    static glm::vec4 literalValue(const Node& n, const Pin& pin) {
        const auto it = n.literals.find(std::string(pin.name));
        glm::vec4 v(pin.x, pin.kind == PinKind::Vector ? pin.y : pin.x, pin.kind == PinKind::Vector ? pin.z : pin.x,
                    pin.kind == PinKind::Vector ? 1.0f : pin.x);
        if (it == n.literals.end()) return v;
        if (it->is_number() || it->is_boolean()) {
            const float x = it->is_boolean() ? (it->get<bool>() ? 1.0f : 0.0f) : it->get<float>();
            return glm::vec4(x, x, x, pin.kind == PinKind::Vector ? 1.0f : x);
        }
        if (it->is_array() && it->size() == 3 && (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number()) {
            return glm::vec4((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>(), 1.0f);
        }
        return v;
    }

    // ---- Registers ----
    Result<std::uint32_t> allocate() {
        for (std::uint32_t r = kFirstTemporary; r < kRegisters; ++r) {
            if (!busy_.contains(r)) {
                busy_.insert(r);
                return r;
            }
        }
        return makeError("the graph is too complex (more than {} values at once)", kRegisters - kFirstTemporary);
    }
    void release(std::uint32_t r) { busy_.erase(r); }

    void emit(Op op, std::uint32_t dst, std::uint32_t a = 0, std::uint32_t b = 0, std::uint32_t c = 0, float p0 = 0.0f,
              float p1 = 0.0f, float p2 = 0.0f) {
        code_->emplace_back(static_cast<float>(op), static_cast<float>(dst), static_cast<float>(a), static_cast<float>(b));
        code_->emplace_back(static_cast<float>(c), p0, p1, p2);
    }
    void emitConst(std::uint32_t dst, glm::vec4 value) {
        code_->emplace_back(static_cast<float>(Op::Const), static_cast<float>(dst), 0.0f, 0.0f);
        code_->push_back(value);
    }

    // ---- Use counts (to free registers once their last user has read them) ----
    Result<> count(int node, std::set<int>& visiting, std::set<int>& done) {
        if (done.contains(node)) return {};
        if (visiting.contains(node)) return makeError("the graph has a cycle");
        visiting.insert(node);
        const Node& n = nodes_.at(node);
        for (const Pin& p : n.type->inputs) {
            const auto it = links_.find({node, std::string(p.name)});
            if (it == links_.end()) continue;
            ++uses_[it->second];
            if (auto r = count(it->second.node, visiting, done); !r) return r;
        }
        visiting.erase(node);
        done.insert(node);
        return {};
    }

    template <std::size_t N>
    Result<> stage(const std::pair<const char*, std::uint32_t> (&outputs)[N], std::vector<glm::vec4>& code) {
        code_ = &code;
        values_.clear();
        uses_.clear();
        busy_.clear();
        std::set<int> visiting, done;
        for (const auto& [pin, reg] : outputs) {
            const auto it = links_.find({output_, pin});
            if (it == links_.end()) continue;
            ++uses_[it->second];
            if (auto r = count(it->second.node, visiting, done); !r) return std::unexpected(r.error());
        }
        const Node& out = nodes_.at(output_);
        for (const auto& [pin, reg] : outputs) {
            const auto it = links_.find({output_, pin});
            if (it == links_.end()) {
                emitConst(reg, literalValue(out, *inputPin(out, pin)));
                continue;
            }
            auto source = value(it->second);
            if (!source) return std::unexpected(source.error());
            emit(Op::Move, reg, *source);
            used(it->second, *source);
        }
        if (code.size() / 2 > kMaxInstructions) return makeError("the graph is too large ({} steps, at most {})", code.size() / 2, kMaxInstructions);
        return {};
    }

    /// One use of a node output has been read: free its register after the last.
    void used(const Source& s, std::uint32_t reg) {
        if (--uses_[s] <= 0) release(reg);
    }

    /// The register holding a node output, computing the node first if needed.
    Result<std::uint32_t> value(const Source& s) {
        if (const auto it = values_.find(s); it != values_.end()) return it->second;
        if (auto r = node(s.node); !r) return std::unexpected(r.error());
        const auto it = values_.find(s);
        if (it == values_.end()) return makeError("node {} has no output '{}'", s.node, s.pin);
        return it->second;
    }

    /// An input as a register: the linked output, the surface property an automatic input stands
    /// for, or the literal (a temporary). `done` releases it after the instruction read it.
    struct Operand {
        std::uint32_t reg = 0;
        std::optional<Source> source;  // linked
    };
    Result<Operand> operand(int node, std::string_view pin) {
        const auto link = links_.find({node, std::string(pin)});
        if (link != links_.end()) {
            auto reg = value(link->second);
            if (!reg) return std::unexpected(reg.error());
            return Operand{*reg, link->second};
        }
        auto reg = allocate();
        if (!reg) return std::unexpected(reg.error());
        const Node& n = nodes_.at(node);
        const Pin* p = inputPin(n, pin);
        if (p != nullptr && p->defaultText == "auto") {
            // UV: the first set; position: the world position.
            emit(Op::Input, *reg, 0, 0, 0, pin == "position" ? 2.0f : 0.0f);
        } else {
            emitConst(*reg, p != nullptr ? literalValue(n, *p) : glm::vec4(0.0f));
        }
        return Operand{*reg, std::nullopt};
    }
    void done(const Operand& o) {
        if (o.source) {
            used(*o.source, o.reg);
        } else {
            release(o.reg);
        }
    }

    Result<int> textureSlot(const Node& n, TextureKind kind) {
        const auto it = n.literals.find("texture");
        const std::string key = it != n.literals.end() && it->is_string() ? it->get<std::string>() : std::string();
        if (key.empty()) return -1;
        const MaterialProgram::Texture t{key, kind};
        for (std::size_t i = 0; i < program_.textures.size(); ++i) {
            if (program_.textures[i] == t) return static_cast<int>(i);
        }
        if (program_.textures.size() >= kMaxTextures) return makeError("the graph uses more than {} textures", kMaxTextures);
        program_.textures.push_back(t);
        return static_cast<int>(program_.textures.size() - 1);
    }

    /// The outputs of `id` that something reads (in this stage).
    bool wanted(int id, std::string_view pin) const {
        const auto it = uses_.find({id, std::string(pin)});
        return it != uses_.end() && it->second > 0;
    }

    Result<> node(int id) {
        const Node& n = nodes_.at(id);
        const std::string_view type = n.type->type;
        auto result = [&](std::string_view pin, std::uint32_t reg) { values_[{id, std::string(pin)}] = reg; };
        // Instructions with operands a, b, c (by input pin names) into one new register.
        auto simple = [&](Op op, std::initializer_list<std::string_view> pins, std::string_view outPin, float p0 = 0.0f) -> Result<> {
            std::vector<Operand> ops;
            for (std::string_view p : pins) {
                auto o = operand(id, p);
                if (!o) return std::unexpected(o.error());
                ops.push_back(*o);
            }
            auto dst = allocate();
            if (!dst) return std::unexpected(dst.error());
            emit(op, *dst, ops.size() > 0 ? ops[0].reg : 0, ops.size() > 1 ? ops[1].reg : 0, ops.size() > 2 ? ops[2].reg : 0, p0);
            for (const Operand& o : ops) done(o);
            result(outPin, *dst);
            return {};
        };
        auto input = [&](float which, std::string_view outPin) -> Result<> {
            auto dst = allocate();
            if (!dst) return std::unexpected(dst.error());
            emit(Op::Input, *dst, 0, 0, 0, which);
            result(outPin, *dst);
            return {};
        };
        // A register's components as separate outputs (splats), the register itself as `main`.
        auto split = [&](std::uint32_t reg, std::string_view main, std::initializer_list<std::pair<std::string_view, int>> parts) -> Result<> {
            for (const auto& [pin, component] : parts) {
                if (!wanted(id, pin)) continue;
                auto dst = allocate();
                if (!dst) return std::unexpected(dst.error());
                emit(Op::Splat, *dst, reg, 0, 0, static_cast<float>(component));
                result(pin, *dst);
            }
            if (!main.empty() && wanted(id, main)) {
                result(main, reg);
            } else {
                release(reg);
            }
            return {};
        };

        if (type == "input.uv") return input(0.0f, "uv");
        if (type == "input.uv2") return input(1.0f, "uv");
        if (type == "input.worldPosition") return input(2.0f, "position");
        if (type == "input.normal") return input(3.0f, "normal");
        if (type == "input.view") return input(4.0f, "view");
        if (type == "input.time") return input(5.0f, "time");
        if (type == "input.objectPosition") return input(6.0f, "position");
        if (type == "const.number" || type == "const.color") {
            auto dst = allocate();
            if (!dst) return std::unexpected(dst.error());
            emitConst(*dst, literalValue(n, n.type->inputs[0]));
            result(n.type->outputs[0].name, *dst);
            return {};
        }
        if (type == "data.reroute") return simple(Op::Move, {"value"}, "value");
        if (type == "texture.color" || type == "texture.data" || type == "texture.normal") {
            const TextureKind kind = type == "texture.color" ? TextureKind::Color : type == "texture.data" ? TextureKind::Data : TextureKind::Normal;
            auto slot = textureSlot(n, kind);
            if (!slot) return std::unexpected(slot.error());
            auto uv = operand(id, "uv");
            if (!uv) return std::unexpected(uv.error());
            std::optional<Operand> strength;
            if (kind == TextureKind::Normal) {
                auto s = operand(id, "strength");
                if (!s) return std::unexpected(s.error());
                strength = *s;
            }
            auto dst = allocate();
            if (!dst) return std::unexpected(dst.error());
            emit(Op::Texture, *dst, uv->reg, strength ? strength->reg : 0, 0, static_cast<float>(*slot),
                 kind == TextureKind::Normal ? 1.0f : 0.0f);
            done(*uv);
            if (strength) done(*strength);
            if (kind == TextureKind::Normal) {
                result("normal", *dst);
                return {};
            }
            if (kind == TextureKind::Color) return split(*dst, "rgb", {{"a", 3}});
            return split(*dst, "rgb", {{"r", 0}, {"g", 1}, {"b", 2}, {"a", 3}});
        }
        if (type == "uv.tiling") return simple(Op::Tiling, {"uv", "tiling", "offset"}, "uv");
        if (type == "uv.panner") return simple(Op::Panner, {"uv", "speed"}, "uv");
        static const std::map<std::string_view, Op> binaries = {
            {"math.add", Op::Add}, {"math.subtract", Op::Subtract}, {"math.multiply", Op::Multiply}, {"math.divide", Op::Divide},
            {"math.min", Op::Min}, {"math.max", Op::Max},           {"math.power", Op::Power},       {"math.dot", Op::Dot}};
        if (const auto it = binaries.find(type); it != binaries.end()) return simple(it->second, {"a", "b"}, "result");
        static const std::map<std::string_view, Op> unaries = {
            {"math.oneMinus", Op::OneMinus}, {"math.abs", Op::Abs},           {"math.frac", Op::Frac},
            {"math.sine", Op::Sine},         {"math.saturate", Op::Saturate}, {"math.normalize", Op::Normalize},
            {"math.length", Op::Length}};
        if (const auto it = unaries.find(type); it != unaries.end()) return simple(it->second, {"value"}, "result");
        if (type == "math.lerp") return simple(Op::Lerp, {"a", "b", "t"}, "result");
        if (type == "math.clamp") return simple(Op::Clamp, {"value", "min", "max"}, "result");
        if (type == "math.smoothstep") return simple(Op::Smoothstep, {"edge0", "edge1", "value"}, "result");
        if (type == "layer.blend") return simple(Op::Lerp, {"base", "layer", "mask"}, "result");
        if (type == "vector.combine") return simple(Op::Combine, {"x", "y", "z"}, "vector");
        if (type == "vector.split") {
            auto v = operand(id, "vector");
            if (!v) return std::unexpected(v.error());
            for (const auto& [pin, component] : {std::pair<std::string_view, int>{"x", 0}, {"y", 1}, {"z", 2}}) {
                if (!wanted(id, pin)) continue;
                auto dst = allocate();
                if (!dst) return std::unexpected(dst.error());
                emit(Op::Splat, *dst, v->reg, 0, 0, static_cast<float>(component));
                result(pin, *dst);
            }
            done(*v);
            return {};
        }
        if (type == "mask.fresnel") return simple(Op::Fresnel, {"power"}, "value");
        if (type == "mask.slope") return simple(Op::Slope, {"min", "max"}, "value");
        if (type == "noise.value") return simple(Op::Noise, {"position", "scale"}, "value");
        if (type == "wind.sway") return simple(Op::Wind, {"strength", "speed", "mask"}, "offset");
        if (type == kOutputNode) return makeError("a Material Output cannot feed another node");
        return makeError("node type '{}' cannot be compiled", type);
    }

    const nlohmann::json& doc_;
    std::map<int, Node> nodes_;
    std::map<std::pair<int, std::string>, Source> links_;  // input (node, pin) -> output feeding it
    int output_ = 0;
    MaterialProgram program_;
    std::vector<glm::vec4>* code_ = nullptr;
    std::map<Source, std::uint32_t> values_;
    std::map<Source, int> uses_;
    std::set<std::uint32_t> busy_;
};

} // namespace

Result<MaterialProgram> compileMaterialGraph(const nlohmann::json& document) { return Compiler(document).run(); }

Result<MaterialProgram> loadMaterialGraph(const std::filesystem::path& file) {
    auto text = fs::readTextFile(file);
    if (!text) return std::unexpected(text.error());
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
    if (doc.is_discarded()) return makeError("{}: not valid JSON", file.filename().string());
    auto program = compileMaterialGraph(doc);
    if (!program) return makeError("{}: {}", file.filename().string(), program.error().message);
    return program;
}

nlohmann::json defaultMaterialGraph() {
    return {{"version", 1},
            {"kind", "material"},
            {"variables", nlohmann::json::array()},
            {"nodes", nlohmann::json::array({{{"id", 1}, {"type", std::string(kOutputNode)}, {"x", 0}, {"y", 0}, {"inputs", nlohmann::json::object()}}})},
            {"links", nlohmann::json::array()}};
}

} // namespace lumora
