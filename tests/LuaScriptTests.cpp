#include "TestUtils.hpp"

#include "runtime/Simulation.hpp"
#include "scene/Hud.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <thread>

using namespace lumora;

namespace {

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

struct LuaWorld {
    test::TempDir dir;
    Scene scene;
    std::unique_ptr<Simulation> sim;

    entt::entity add(const std::string& name, const std::string& behaviour, const std::string& parameters = {}) {
        const entt::entity e = scene.createEntity(name);
        auto& script = scene.registry().emplace<ScriptComponent>(e);
        script.behaviour = behaviour;
        script.parameters = parameters;
        return e;
    }
    void start() {
        sim = std::make_unique<Simulation>(scene);
        sim->setProjectRoot(dir.path());
        sim->setScripts(nullptr, nullptr);  // no game library: Lua only
    }
    glm::vec3 position(entt::entity e) { return scene.registry().get<TransformComponent>(e).position; }
};

} // namespace

TEST(LuaScript, BehaviourMovesItsEntityAndReadsParameters) {
    LuaWorld w;
    write(w.dir.path() / "Scripts/Mover.lua", R"(
        local Mover = Behaviour:extend()
        function Mover:start()
            self.speed = self:param("speed", 1)
            self:setPosition(vec3(0, 1, 0))
        end
        function Mover:update(dt)
            self:moveBy(vec3(self.speed * dt, 0, 0))
        end
        return Mover
    )");
    const entt::entity e = w.add("Box", "Scripts/Mover.lua", "speed=4");
    w.start();
    for (int i = 0; i < 60; ++i) w.sim->step();
    const glm::vec3 p = w.position(e);
    EXPECT_NEAR(p.x, 4.0f * static_cast<float>(w.sim->simulatedSeconds()), 1e-3f);
    EXPECT_FLOAT_EQ(p.y, 1.0f);
    EXPECT_EQ(w.sim->scriptInstances(), 1u);
}

TEST(LuaScript, PlainTablesTimersRequireAndOtherEntities) {
    LuaWorld w;
    write(w.dir.path() / "Scripts/util/numbers.lua", "return { double = function(x) return x * 2 end }");
    write(w.dir.path() / "Scripts/Boss.lua", R"(
        local numbers = require("util.numbers")
        local Boss = {}                       -- a plain table also works
        function Boss:start()
            self.ticks = 0
            self:every(0.1, function(me) me.ticks = me.ticks + 1 end)
            self:after(0.25, function(me)
                local target = me:find("Target")
                me:setPositionOf(target, vec3(numbers.double(21), 0, 0))
            end)
        end
        function Boss:update(dt)
            self:setPosition(vec3(self.ticks, 0, 0))
        end
        return Boss
    )");
    const entt::entity boss = w.add("Boss", "Scripts/Boss.lua");
    const entt::entity target = w.scene.createEntity("Target");
    w.start();
    const int steps = static_cast<int>(std::ceil(0.55 / w.sim->fixedStep()));
    for (int i = 0; i < steps; ++i) w.sim->step();
    EXPECT_FLOAT_EQ(w.position(target).x, 42.0f) << "after() ran once, require() loaded the module";
    EXPECT_NEAR(w.position(boss).x, 5.0f, 1.0f) << "every(0.1) ticked about five times";
}

TEST(LuaScript, ErrorsDisableOnlyTheBrokenBehaviour) {
    LuaWorld w;
    write(w.dir.path() / "Scripts/Broken.lua", R"(
        local B = Behaviour:extend()
        function B:update(dt) error("boom") end
        return B
    )");
    write(w.dir.path() / "Scripts/Syntax.lua", "local B = {  -- missing brace");
    write(w.dir.path() / "Scripts/Good.lua", R"(
        local G = Behaviour:extend()
        function G:update(dt) self:moveBy(vec3(0, 0, 1)) end
        return G
    )");
    w.add("A", "Scripts/Broken.lua");
    w.add("B", "Scripts/Syntax.lua");
    w.add("C", "Scripts/Missing.lua");
    const entt::entity good = w.add("D", "Scripts/Good.lua");
    w.start();
    for (int i = 0; i < 3; ++i) w.sim->step();
    EXPECT_FLOAT_EQ(w.position(good).z, 3.0f);
    EXPECT_EQ(w.sim->scriptInstances(), 1u) << "the failing behaviours were disabled";
}

TEST(LuaScript, SandboxHasNoFileOrProcessAccess) {
    LuaWorld w;
    write(w.dir.path() / "Scripts/Probe.lua", R"(
        local P = Behaviour:extend()
        function P:start()
            local hidden = (io == nil) and (os.execute == nil) and (dofile == nil) and (loadfile == nil)
                           and (package == nil) and (os.clock ~= nil)
            self:setPosition(vec3(hidden and 1 or -1, 0, 0))
            local ok = pcall(require, "../../etc/passwd")
            self:setScale(vec3(ok and -1 or 1, 1, 1))
        end
        return P
    )");
    const entt::entity e = w.add("Probe", "Scripts/Probe.lua");
    w.start();
    w.sim->step();
    EXPECT_FLOAT_EQ(w.position(e).x, 1.0f);
    EXPECT_FLOAT_EQ(w.scene.registry().get<TransformComponent>(e).scale.x, 1.0f) << "require cannot leave Scripts/";
}

TEST(LuaScript, EndlessLoopIsAbortedByTheWatchdog) {
    LuaWorld w;
    write(w.dir.path() / "Scripts/Loop.lua", R"(
        local L = Behaviour:extend()
        function L:update(dt) while true do end end
        return L
    )");
    w.add("Loop", "Scripts/Loop.lua");
    w.start();
    // The default limit is 5 s; the test only checks that the step returns and the behaviour is gone.
    const auto begin = std::chrono::steady_clock::now();
    w.sim->step();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(30));
    EXPECT_EQ(w.sim->scriptInstances(), 0u);
}

TEST(LuaScript, SavingTheFileReloadsTheBehaviour) {
    LuaWorld w;
    const auto file = w.dir.path() / "Scripts/Hot.lua";
    write(file, "local H = Behaviour:extend() function H:update() self:setPosition(vec3(1, 0, 0)) end return H");
    const entt::entity e = w.add("Hot", "Scripts/Hot.lua");
    w.start();
    w.sim->step();
    EXPECT_FLOAT_EQ(w.position(e).x, 1.0f);
    write(file, "local H = Behaviour:extend() function H:update() self:setPosition(vec3(2, 0, 0)) end return H");
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(5));
    const int steps = static_cast<int>(std::ceil(1.0 / w.sim->fixedStep()));
    for (int i = 0; i < steps; ++i) w.sim->step();
    EXPECT_FLOAT_EQ(w.position(e).x, 2.0f) << "the new code runs after the file changed";
}

TEST(LuaScript, DrawsOnTheHud) {
    LuaWorld w;
    write(w.dir.path() / "Scripts/Hud.lua", R"(
        local H = Behaviour:extend()
        function H:update()
            self:drawRect(10, 10, 100, 20, vec3(1, 0, 0), 0.5)
            self:drawText("Score: " .. 3, 10, 40)
        end
        return H
    )");
    w.add("Hud", "Scripts/Hud.lua");
    w.start();
    w.sim->step();
    ASSERT_EQ(w.sim->hud().rects.size(), 1u);
    ASSERT_EQ(w.sim->hud().texts.size(), 1u);
    EXPECT_EQ(w.sim->hud().texts[0].utf8, "Score: 3");
    EXPECT_FLOAT_EQ(w.sim->hud().rects[0].color.a, 0.5f);
}
