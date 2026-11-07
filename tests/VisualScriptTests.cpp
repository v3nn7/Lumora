#include "TestUtils.hpp"

#include "runtime/AnimatorStateMachine.hpp"
#include "runtime/Simulation.hpp"
#include "runtime/VisualScript.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <clocale>
#include <nlohmann/json.hpp>

#include <fstream>
#include <set>

using namespace lumora;
using nlohmann::json;

namespace {

struct GraphWorld {
    test::TempDir dir;
    Scene scene;
    std::unique_ptr<Simulation> sim;

    entt::entity add(const std::string& name, const json& graph, const std::string& file = "Content/Logic.lgraph") {
        std::filesystem::create_directories((dir.path() / file).parent_path());
        std::ofstream(dir.path() / file) << graph.dump(2);
        const entt::entity e = scene.createEntity(name);
        scene.registry().emplace<ScriptComponent>(e).behaviour = file;
        return e;
    }
    void start() {
        sim = std::make_unique<Simulation>(scene);
        sim->setProjectRoot(dir.path());
        sim->setScripts(nullptr, nullptr);
    }
    glm::vec3 position(entt::entity e) { return scene.registry().get<TransformComponent>(e).position; }
};

json node(int id, const char* type, json inputs = json::object()) {
    return {{"id", id}, {"type", type}, {"x", 0}, {"y", 0}, {"inputs", std::move(inputs)}};
}
json link(int from, const char* fromPin, int to, const char* toPin) {
    return {{"from", from}, {"fromPin", fromPin}, {"to", to}, {"toPin", toPin}};
}

} // namespace

TEST(VisualScript, LibraryIsConsistent) {
    std::set<std::string_view> types;
    for (const visual::NodeDef& d : visual::nodeLibrary()) {
        EXPECT_TRUE(types.insert(d.type).second) << "duplicate node type " << d.type;
        EXPECT_FALSE(d.title.empty()) << d.type;
        EXPECT_TRUE(static_cast<bool>(d.run)) << d.type;
        if (d.pure) {
            for (const auto& p : d.inputs) EXPECT_NE(p.type, visual::PinType::Exec) << d.type;
            for (const auto& p : d.outputs) EXPECT_NE(p.type, visual::PinType::Exec) << d.type;
        } else if (!d.event) {
            ASSERT_FALSE(d.inputs.empty()) << d.type;
            EXPECT_EQ(d.inputs[0].type, visual::PinType::Exec) << d.type << " needs an exec input first";
        }
    }
    EXPECT_GT(types.size(), 100u);
}

TEST(VisualScript, UpdateMovesTheEntityWithMathNodes) {
    // On Update -> Move By (offset = Make Vector(x = dt * speed)).
    GraphWorld w;
    const json graph = {
        {"version", 1},
        {"variables", json::array({{{"name", "speed"}, {"type", "number"}, {"value", 3}}})},
        {"nodes", json::array({node(1, "event.update"), node(2, "entity.translate"), node(3, "vec.make"),
                               node(4, "math.mul"), node(5, "var.get", {{"name", "speed"}})})},
        {"links", json::array({link(1, "exec", 2, "in"), link(3, "vector", 2, "offset"), link(4, "result", 3, "x"),
                               link(1, "dt", 4, "a"), link(5, "value", 4, "b")})},
    };
    const entt::entity e = w.add("Mover", graph);
    w.start();
    for (int i = 0; i < 30; ++i) w.sim->step();
    EXPECT_NEAR(w.position(e).x, 3.0f * static_cast<float>(w.sim->simulatedSeconds()), 1e-3f);
    EXPECT_EQ(w.sim->scriptInstances(), 1u);
}

TEST(VisualScript, FlowNodesBranchLoopAndDelay) {
    GraphWorld w;
    // On Start: For 1..4 -> Add To Variable(count, index); Completed -> Branch(count == 10)
    //   true -> Set Position(10,0,0). Then Delay 0.2 -> Set Scale(2,2,2).
    const json graph = {
        {"variables", json::array({{{"name", "count"}, {"type", "number"}, {"value", 0}}})},
        {"nodes", json::array({
            node(1, "event.start"),
            node(2, "flow.for", {{"first", 1}, {"last", 4}}),
            node(3, "var.add", {{"name", "count"}}),
            node(4, "flow.branch"),
            node(5, "logic.equal", {{"b", 10}}),
            node(6, "var.get", {{"name", "count"}}),
            node(7, "entity.setPosition", {{"value", json::array({10, 0, 0})}}),
            node(8, "flow.sequence"),
            node(9, "flow.delay", {{"seconds", 0.2}}),
            node(10, "entity.setScale", {{"value", json::array({2, 2, 2})}}),
        })},
        {"links", json::array({
            link(1, "exec", 8, "in"), link(8, "then0", 2, "in"), link(2, "body", 3, "in"), link(2, "index", 3, "amount"),
            link(2, "completed", 4, "in"), link(6, "value", 5, "a"), link(5, "result", 4, "condition"),
            link(4, "true", 7, "in"), link(8, "then1", 9, "in"), link(9, "completed", 10, "in"),
        })},
    };
    const entt::entity e = w.add("Flow", graph);
    w.start();
    w.sim->step();
    EXPECT_FLOAT_EQ(w.position(e).x, 10.0f) << "1+2+3+4 == 10 took the true branch";
    EXPECT_FLOAT_EQ(w.scene.registry().get<TransformComponent>(e).scale.x, 1.0f) << "the delay has not elapsed";
    for (int i = 0; i < static_cast<int>(0.3 / w.sim->fixedStep()); ++i) w.sim->step();
    EXPECT_FLOAT_EQ(w.scene.registry().get<TransformComponent>(e).scale.x, 2.0f);
}

TEST(VisualScript, EntityLiteralsFindByNameAndEndlessLoopsStop) {
    GraphWorld w;
    const entt::entity door = w.scene.createEntity("Door");
    const json opener = {
        {"nodes", json::array({node(1, "event.start"),
                               node(2, "entity.setPosition", {{"entity", "Door"}, {"value", json::array({0, 5, 0})}})})},
        {"links", json::array({link(1, "exec", 2, "in")})},
    };
    w.add("Opener", opener, "Content/Opener.lgraph");
    // A Sequence whose output runs itself: an endless loop that must be stopped, not hang.
    const json loop = {
        {"nodes", json::array({node(1, "event.update"), node(2, "flow.sequence")})},
        {"links", json::array({link(1, "exec", 2, "in"), link(2, "then0", 2, "in")})},
    };
    w.add("Loop", loop, "Content/Loop.lgraph");
    w.start();
    w.sim->step();
    EXPECT_FLOAT_EQ(w.position(door).y, 5.0f);
    EXPECT_EQ(w.sim->scriptInstances(), 1u) << "the looping graph was disabled, the other keeps running";
}

TEST(VisualScript, BadLinksAndUnknownNodesAreSkippedWithWarnings) {
    const json graph = {
        {"nodes", json::array({node(1, "event.start"), node(2, "no.such.node"), node(3, "debug.log"), node(4, "math.add")})},
        {"links", json::array({link(1, "exec", 2, "in"), link(1, "exec", 4, "a"), link(4, "result", 3, "nope"),
                               link(1, "exec", 3, "in")})},
    };
    std::vector<std::string> warnings;
    auto parsed = visual::Graph::parse(graph, &warnings);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)->nodes().size(), 3u);
    EXPECT_EQ(warnings.size(), 4u);
    EXPECT_EQ((*parsed)->nodes()[0].execTargets[0].size(), 1u) << "only the valid exec link remains";
}

TEST(VisualScript, ValuesConvert) {
    using visual::Value;
    EXPECT_EQ(Value::ofNumber(3).asString(), "3");
    EXPECT_EQ(Value::ofNumber(0.5).asString(), "0.500");
    EXPECT_TRUE(Value::ofNumber(2).asBool());
    EXPECT_FLOAT_EQ(Value::ofString("1, 2, 3").asVec3().z, 3.0f);
    EXPECT_FLOAT_EQ(Value::ofNumber(2).asVec3().y, 2.0f);
    EXPECT_EQ(Value::fromJson(json::array({1, 2, 3}), visual::PinType::Vec3).toJson(), json::array({1.0, 2.0, 3.0}));
}

TEST(AnimatorStateMachine, TransitionsFollowParametersTriggersAndExitTime) {
    const json doc = json::parse(R"({
      "parameters": [ { "name": "speed", "type": "float" }, { "name": "jump", "type": "trigger" } ],
      "entry": "Idle",
      "states": [ { "name": "Idle" }, { "name": "Run", "speed": 1.5 }, { "name": "Jump", "loop": false } ],
      "transitions": [
        { "from": "Idle", "to": "Run", "duration": 0.2, "conditions": [ { "parameter": "speed", "op": ">", "value": 0.1 } ] },
        { "from": "Run", "to": "Idle", "conditions": [ { "parameter": "speed", "op": "<=", "value": 0.1 } ] },
        { "from": "*", "to": "Jump", "duration": 0.1, "conditions": [ { "parameter": "jump", "op": "trigger" } ] },
        { "from": "Jump", "to": "Idle", "exitTime": 1.0 }
      ] })");
    auto parsed = AnimatorStateMachine::parse(doc);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    AnimatorStateMachine m = std::move(*parsed);
    EXPECT_EQ(m.current()->name, "Idle");
    EXPECT_FALSE(m.update(0.5f, false).has_value());
    ASSERT_TRUE(m.setParameter("speed", 0, 2.0f));
    auto change = m.update(0.5f, false);
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->state->name, "Run");
    EXPECT_FLOAT_EQ(change->state->speed, 1.5f);
    EXPECT_FLOAT_EQ(change->fadeSeconds, 0.2f);
    m.setParameter("jump", 2, 1.0f);
    ASSERT_EQ(m.update(0.1f, false)->state->name, "Jump");
    EXPECT_FLOAT_EQ(m.parameter("jump"), 0.0f) << "the trigger was consumed";
    EXPECT_FALSE(m.update(0.5f, false).has_value()) << "waits for the exit time";
    EXPECT_EQ(m.update(1.0f, true)->state->name, "Idle");
    EXPECT_FALSE(m.setParameter("missing", 0, 1.0f));

    EXPECT_FALSE(AnimatorStateMachine::parse(json::parse(R"({"states": []})")).has_value());
    EXPECT_FALSE(AnimatorStateMachine::parse(json::parse(
        R"({"states": [{"name": "A"}], "transitions": [{"from": "A", "to": "B"}]})")).has_value());
}

TEST(VisualScript, TextToNumbersIgnoresTheProcessLocale) {
    // A decimal-comma locale (Polish) must not change how "2.5" or "1,0.5,2" read.
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous != nullptr ? previous : "C";
    const bool comma = std::setlocale(LC_NUMERIC, "pl_PL.UTF-8") != nullptr || std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr;
    EXPECT_DOUBLE_EQ(visual::Value::ofString(" 2.5").asNumber(), 2.5);
    EXPECT_DOUBLE_EQ(visual::Value::ofString("abc").asNumber(), 0.0);
    const auto v = visual::Value::ofString("1, 0.5,2").asVec3();
    EXPECT_FLOAT_EQ(v.x, 1.0f);
    EXPECT_FLOAT_EQ(v.y, 0.5f);
    EXPECT_FLOAT_EQ(v.z, 2.0f);
    const auto bad = visual::Value::ofString("1 2 3").asVec3();
    EXPECT_FLOAT_EQ(bad.x, 0.0f) << "commas separate the components";
    std::setlocale(LC_NUMERIC, saved.c_str());
    if (!comma) GTEST_SKIP() << "no decimal-comma locale installed to test with";
}

#include "runtime/UiDocument.hpp"

TEST(VisualScript, UiDocumentAndWidgetNodes) {
    // On Start: Open UI; on Update: a checkbox and a slider (their outputs into texts of the
    // document) and Set UI Text with the slider's value.
    GraphWorld w;
    UiDocument doc = defaultUiDocument();
    UiWidget label;
    label.type = UiWidget::Type::Label;
    label.name = "Status";
    label.text = "-";
    doc.root.children.push_back(label);
    ASSERT_TRUE(saveUiDocument(doc, w.dir.path() / "Content" / "UI" / "Hud.lui").has_value());
    const json graph = {
        {"version", 1},
        {"nodes",
         {node(1, "event.start"), node(2, "uidoc.open", {{"document", "Content/UI/Hud.lui"}}), node(3, "event.update"),
          node(4, "ui.checkbox", {{"text", "Fast"}, {"initial", true}}), node(5, "ui.slider", {{"text", "Speed"}, {"min", 0}, {"max", 10}, {"initial", 4}}),
          node(6, "uidoc.setText", {{"name", "Status"}}), node(7, "text.number", {{"decimals", 0}})}},
        {"links",
         {link(1, "exec", 2, "in"), link(3, "exec", 4, "in"), link(4, "then", 5, "in"), link(5, "then", 6, "in"),
          link(5, "value", 7, "value"), link(7, "text", 6, "text")}}};
    w.add("Logic", graph);
    w.start();
    w.sim->step();
    w.sim->step();
    bool status = false, fast = false;
    for (const HudCanvas::Text& t : w.sim->hud().texts) {
        if (t.utf8 == "4") status = true;  // the slider's initial value, through Set UI Text
        if (t.utf8 == "Fast") fast = true;
    }
    EXPECT_TRUE(status) << "Set UI Text wrote the slider's value into the open document";
    EXPECT_TRUE(fast) << "the checkbox is drawn";
}
