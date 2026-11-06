// Gameplay scripting end to end: real C++ scripts compiled with CMake and the system compiler,
// loaded into a simulation, changed, rebuilt and hot-reloaded while it runs.
#include "TestUtils.hpp"

#include "common/GameExporter.hpp"
#include "common/ScriptBuilder.hpp"
#include "project/Project.hpp"
#include "runtime/PhysicsWorld.hpp"
#include "scene/Prefab.hpp"
#include "scene/SceneSerializer.hpp"
#include "runtime/Scripting.hpp"
#include "runtime/Simulation.hpp"
#include "scene/Scene.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QFile>
#include <QStandardPaths>

#include <gtest/gtest.h>

#include <fstream>

using namespace lumora;

namespace {

void writeScript(const std::filesystem::path& project, const char* file, const std::string& source) {
    std::filesystem::create_directories(project / "Scripts");
    QFile f(QString::fromStdString((project / "Scripts" / file).string()));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QByteArray::fromStdString(source));
}

/// Moves its entity along `axis` at 1 m/s and reports through the engine API.
std::string mover(const char* axis) {
    return std::string(R"cpp(#include <lumora/Script.hpp>
#include <stdexcept>
using namespace lumora::script;
class Mover : public Behaviour {
public:
    void start() override { log("Mover start on " + name()); }
    void update(float dt) override { setPosition(position() + Vec3)cpp") + axis + R"cpp( * dt); }
};
LUMORA_BEHAVIOUR(Mover);

class Thrower : public Behaviour {
public:
    void update(float) override { if (time() > 0.05) throw std::runtime_error("boom"); }
};
LUMORA_BEHAVIOUR(Thrower);
)cpp";
}

class ScriptTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (QStandardPaths::findExecutable(QStringLiteral("cmake")).isEmpty()) {
            GTEST_SKIP() << "CMake is not installed: scripts cannot be built";
        }
    }
    test::TempDir dir;
};

} // namespace

TEST_F(ScriptTest, BuildsRunsAndHotReloads) {
    const std::filesystem::path project = dir.path();
    writeScript(project, "Mover.cpp", mover("{1.0f, 0.0f, 0.0f}"));
    app::ScriptBuilder builder(QString::fromStdString(project.string()));
    ASSERT_TRUE(builder.hasScripts());
    QString log;
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();

    auto module = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_GE((*module)->find("Mover"), 0);
    EXPECT_GE((*module)->find("Thrower"), 0);

    Scene scene("Test");
    const entt::entity box = scene.createEntity("Box");
    scene.registry().emplace<ScriptComponent>(box).behaviour = "Mover";
    const entt::entity bomb = scene.createEntity("Bomb");
    scene.registry().emplace<ScriptComponent>(bomb).behaviour = "Thrower";
    const entt::entity unknown = scene.createEntity("Unknown");
    scene.registry().emplace<ScriptComponent>(unknown).behaviour = "DoesNotExist";

    Simulation sim(scene);
    sim.setScripts(*module, nullptr);
    for (int i = 0; i < 60; ++i) sim.step();  // one second
    const glm::vec3 afterFirst = scene.registry().get<TransformComponent>(box).position;
    EXPECT_NEAR(afterFirst.x, 1.0f, 1e-3f) << "Mover ran every fixed step";
    EXPECT_NEAR(afterFirst.y, 0.0f, 1e-6f);
    EXPECT_EQ(sim.scriptInstances(), 1u) << "Thrower is disabled after its exception, the unknown name is skipped";

    // Change the script while the simulation keeps its state, rebuild, hot-reload.
    writeScript(project, "Mover.cpp", mover("{0.0f, 2.0f, 0.0f}"));
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();
    auto reloaded = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().message;
    sim.setScripts(*reloaded, nullptr);
    module->reset();  // the simulation now holds the only references
    for (int i = 0; i < 30; ++i) sim.step();  // half a second at 2 m/s up
    const glm::vec3 afterReload = scene.registry().get<TransformComponent>(box).position;
    EXPECT_NEAR(afterReload.x, afterFirst.x, 1e-4f) << "the reload keeps the scene as it was";
    EXPECT_NEAR(afterReload.y, 1.0f, 1e-3f) << "the new code runs";
}

TEST_F(ScriptTest, CompileErrorsAreReported) {
    writeScript(dir.path(), "Broken.cpp", "#include <lumora/Script.hpp>\nthis is not C++;\n");
    app::ScriptBuilder builder(QString::fromStdString(dir.path().string()));
    QString log;
    EXPECT_FALSE(builder.buildBlocking(&log));
    EXPECT_TRUE(log.contains(QStringLiteral("Broken.cpp"))) << log.toStdString();
    EXPECT_FALSE(QFile::exists(builder.libraryPath()));
}

TEST_F(ScriptTest, NewBehaviourTemplateCompiles) {
    app::ScriptBuilder builder(QString::fromStdString(dir.path().string()));
    QString error;
    ASSERT_TRUE(builder.createBehaviour(QStringLiteral("Spinner"), &error)) << error.toStdString();
    EXPECT_FALSE(builder.createBehaviour(QStringLiteral("Spinner"), &error)) << "never overwrites a script";
    EXPECT_FALSE(builder.createBehaviour(QStringLiteral("not a name"), &error));
    QString log;
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();
    auto module = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_EQ((*module)->behaviours(), std::vector<std::string>{"Spinner"});
}

TEST_F(ScriptTest, PhysicsEntitiesAndParameters) {
    const std::filesystem::path project = dir.path();
    writeScript(project, "Game.cpp", R"cpp(#include <lumora/Script.hpp>
using namespace lumora::script;

// Pushed sideways at start; reports its first collision by moving the "Flag" entity.
class Kicker : public Behaviour {
public:
    void start() override { addImpulse(Vec3{param("impulse", 0.0f), 0.0f, 0.0f}); }
    void onCollision(Entity other) override {
        if (reported_) return;
        reported_ = true;
        setPositionOf(find("Flag"), Vec3{name(other) == "Ground" ? 1.0f : -1.0f, 0.0f, 0.0f});
    }
private:
    bool reported_ = false;
};
LUMORA_BEHAVIOUR(Kicker);

// Drops itself onto whatever lies below.
class Prober : public Behaviour {
public:
    void start() override {
        RayHit hit;
        if (raycast(position(), Vec3{0.0f, -1.0f, 0.0f}, 50.0f, hit)) setPosition(hit.point);
    }
};
LUMORA_BEHAVIOUR(Prober);

// Clones the "Template" entity once; the clone's Lifetime script removes it again.
class Spawner : public Behaviour {
public:
    void update(float) override {
        if (!spawned_ && time() >= 0.1) {
            spawned_ = true;
            Entity copy = clone(find("Template"));
            setPositionOf(copy, Vec3{0.0f, 5.0f, 0.0f});
        }
    }
private:
    bool spawned_ = false;
};
LUMORA_BEHAVIOUR(Spawner);

class Lifetime : public Behaviour {
public:
    void start() override { born_ = time(); }
    void update(float) override {
        if (position().y > 1.0f && time() - born_ > param("life", 1.0f)) destroy(entity());  // the template stays at y = 0
    }
private:
    double born_ = 0.0;
};
LUMORA_BEHAVIOUR(Lifetime);
)cpp");
    app::ScriptBuilder builder(QString::fromStdString(project.string()));
    QString log;
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();
    auto module = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(module.has_value()) << module.error().message;

    Scene scene("Test");
    auto& reg = scene.registry();
    const entt::entity ground = scene.createEntity("Ground");
    reg.emplace<ColliderComponent>(ground).size = glm::vec3(40.0f, 1.0f, 40.0f);  // top at y = 0.5
    const entt::entity ball = scene.createEntity("Ball");
    reg.get<TransformComponent>(ball).position = glm::vec3(0.0f, 2.0f, 0.0f);
    reg.emplace<ColliderComponent>(ball).shape = ColliderShape::Sphere;
    reg.emplace<RigidBodyComponent>(ball);
    reg.emplace<ScriptComponent>(ball) = ScriptComponent{"Kicker", "impulse = 3"};
    const entt::entity flag = scene.createEntity("Flag");
    const entt::entity probe = scene.createEntity("Probe");
    reg.get<TransformComponent>(probe).position = glm::vec3(5.0f, 10.0f, 5.0f);
    reg.emplace<ScriptComponent>(probe).behaviour = "Prober";
    const entt::entity spawner = scene.createEntity("Spawner");
    reg.emplace<ScriptComponent>(spawner).behaviour = "Spawner";
    const entt::entity templ = scene.createEntity("Template");
    reg.emplace<ScriptComponent>(templ) = ScriptComponent{"Lifetime", "life=0.2"};
    scene.updateWorldTransforms();

    Simulation sim(scene);
    sim.setScripts(*module, nullptr);
    ASSERT_NE(sim.physics(), nullptr);
    const std::size_t entitiesAtStart = scene.entityCount();

    sim.step();
    EXPECT_NEAR(reg.get<TransformComponent>(probe).position.y, 0.5f, 1e-3f) << "raycast hit the ground's top";
    EXPECT_GT(sim.physics()->linearVelocity(ball).x, 2.0f) << "the impulse (3 N s on 1 kg) moved the ball";

    for (int i = 0; i < 9; ++i) sim.step();  // t = 0.17 s: the clone exists
    EXPECT_EQ(scene.entityCount(), entitiesAtStart + 1) << "Spawner cloned the template";
    EXPECT_EQ(sim.scriptInstances(), 5u) << "the clone runs its own Lifetime behaviour";

    for (int i = 0; i < 50; ++i) sim.step();  // 1 s in total: the ball landed, the clone expired
    EXPECT_EQ(scene.entityCount(), entitiesAtStart) << "the clone destroyed itself after life=0.2 s";
    EXPECT_EQ(sim.scriptInstances(), 4u);
    EXPECT_FLOAT_EQ(reg.get<TransformComponent>(flag).position.x, 1.0f) << "onCollision named the ground";
    EXPECT_GT(reg.get<TransformComponent>(ball).position.x, 1.0f);
}

TEST(GameExport, WritesARunnableStandaloneGame) {
    test::TempDir dir;
    auto project = Project::create(dir.path() / "Demo", "Demo", ProjectTemplate::BasicScene);
    ASSERT_TRUE(project.has_value()) << project.error().message;
    std::filesystem::create_directories(dir.path() / "Demo" / "Saved" / "Cache");
    std::ofstream(dir.path() / "Demo" / "Saved" / "Cache" / "junk.bin") << "editor state";
    std::filesystem::create_directories(dir.path() / "Demo" / "Saved" / "ScriptBuild" / "bin");
    std::ofstream(dir.path() / "Demo" / "Saved" / "ScriptBuild" / "bin" / "game.so") << "library";

    const std::filesystem::path out = dir.path() / "Export";
    auto launcher = app::exportGame({dir.path() / "Demo", "Demo", out, {}});
    ASSERT_TRUE(launcher.has_value()) << launcher.error().message;
    EXPECT_TRUE(std::filesystem::exists(*launcher));
    EXPECT_TRUE(std::filesystem::exists(out / "Game" / "Demo.lumora"));
    EXPECT_TRUE(std::filesystem::exists(out / "Game" / "Content" / "Scenes" / "Main.lscene"));
    EXPECT_FALSE(std::filesystem::exists(out / "Game" / "Saved" / "Cache")) << "editor state stays behind";
    EXPECT_TRUE(std::filesystem::exists(out / "Game" / "Saved" / "ScriptBuild" / "bin" / "game.so"));
    EXPECT_FALSE(app::exportGame({dir.path() / "Demo", "Demo", out, {}}).has_value()) << "never overwrites";

    // The exported runtime opens the exported game on its own.
#if defined(_WIN32)
    const QString player = QString::fromStdString((out / "bin" / "lumora-editor.exe").string());
#else
    const QString player = QString::fromStdString((out / "bin" / "lumora-editor").string());
#endif
    QProcess run;
    run.start(player, {QStringLiteral("--headless"), QStringLiteral("--project"),
                       QString::fromStdString((out / "Game").string()), QStringLiteral("--validate")});
    ASSERT_TRUE(run.waitForFinished(60000));
    EXPECT_EQ(run.exitCode(), 0) << run.readAllStandardError().toStdString();
    EXPECT_TRUE(run.readAllStandardOutput().contains("Project 'Demo' OK"));
}

TEST_F(ScriptTest, SpawnsPrefabsWithTheirOwnBehaviours) {
    const std::filesystem::path project = dir.path();
    writeScript(project, "Spawn.cpp", R"cpp(#include <lumora/Script.hpp>
using namespace lumora::script;
class Launcher : public Behaviour {
public:
    void update(float) override {
        if (!done_ && time() >= 0.05) {
            done_ = true;
            spawned_ = spawn("Content/Prefabs/Rocket.lprefab", Vec3{2.0f, 1.0f, 0.0f});
            if (spawned_ == kNoEntity) warn("spawn failed");
        }
    }
private:
    bool done_ = false;
    Entity spawned_ = kNoEntity;
};
LUMORA_BEHAVIOUR(Launcher);

class Rise : public Behaviour {
public:
    void update(float dt) override { setPosition(position() + Vec3{0.0f, dt, 0.0f}); }
};
LUMORA_BEHAVIOUR(Rise);
)cpp");
    {
        Scene source("Prefab source");
        const entt::entity rocket = source.createEntity("Rocket");
        source.registry().emplace<ScriptComponent>(rocket).behaviour = "Rise";
        (void)source.createEntity("Flame", Uuid::generate(), rocket);
        ASSERT_TRUE(savePrefab(source, rocket, project / "Content" / "Prefabs" / "Rocket.lprefab").has_value());
    }
    app::ScriptBuilder builder(QString::fromStdString(project.string()));
    QString log;
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();
    auto module = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(module.has_value()) << module.error().message;

    Scene scene("Level");
    scene.registry().emplace<ScriptComponent>(scene.createEntity("Pad")).behaviour = "Launcher";
    Simulation sim(scene);
    sim.setProjectRoot(project);
    sim.setScripts(*module, nullptr);
    for (int i = 0; i < 4; ++i) sim.step();  // t = 0.067 s: spawned at the end of step 4
    ASSERT_EQ(scene.entityCount(), 3u) << "rocket and its flame";
    entt::entity rocket = entt::null;
    for (auto [e, name] : scene.registry().view<NameComponent>().each()) {
        if (name.name == "Rocket") rocket = e;
    }
    ASSERT_TRUE(rocket != entt::null);
    EXPECT_EQ(sim.scriptInstances(), 2u) << "the spawned rocket runs its own Rise behaviour";
    for (int i = 0; i < 60; ++i) sim.step();
    const glm::vec3 p = scene.registry().get<TransformComponent>(rocket).position;
    EXPECT_NEAR(p.x, 2.0f, 1e-5f);
    EXPECT_NEAR(p.y, 2.0f, 0.05f) << "spawned at y = 1, rose 1 m in a second";
}

TEST_F(ScriptTest, PlatformerTemplatePlays) {
    // The template end to end: its scripts compile, the player stands, collects a coin it walks
    // into, the HUD counts it, and the camera follows.
    auto project = Project::create(dir.path() / "Jump", "Jump", ProjectTemplate::Platformer);
    ASSERT_TRUE(project.has_value()) << project.error().message;
    ASSERT_TRUE(std::filesystem::exists(dir.path() / "Jump" / "Scripts" / "Game.cpp"));
    app::ScriptBuilder builder(QString::fromStdString((dir.path() / "Jump").string()));
    QString log;
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();
    auto module = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(module.has_value()) << module.error().message;
    auto scene = SceneSerializer::load(project->defaultScenePath());
    ASSERT_TRUE(scene.has_value()) << scene.error().message;
    Scene& level = **scene;
    auto named = [&](const char* name) {
        std::vector<entt::entity> found;
        for (auto [e, n] : level.registry().view<NameComponent>().each()) {
            if (n.name == name) found.push_back(e);
        }
        return found;
    };
    const entt::entity player = named("Player").at(0);
    const entt::entity camera = named("Main Camera").at(0);
    const std::size_t coins = named("Coin").size();
    ASSERT_EQ(coins, 8u);

    Simulation sim(level);
    sim.setProjectRoot(dir.path() / "Jump");
    sim.setScripts(*module, nullptr);
    for (int i = 0; i < 60; ++i) sim.step();
    ASSERT_NE(sim.physics(), nullptr);
    EXPECT_TRUE(sim.physics()->characterGrounded(player)) << "the player stands on the ground";
    ASSERT_FALSE(sim.hud().texts.empty());
    EXPECT_EQ(sim.hud().texts[0].utf8, "Coins: 0 / 8");
    const glm::vec3 cameraBefore = level.registry().get<TransformComponent>(camera).position;

    // Put the player next to the coin at (-4, 1, 2), in reach of its body.
    level.registry().get<TransformComponent>(player).position = glm::vec3(-4.0f, 0.0f, 2.3f);
    for (int i = 0; i < 30; ++i) sim.step();
    EXPECT_EQ(named("Coin").size(), coins - 1) << "collected and removed";
    EXPECT_EQ(sim.hud().texts[0].utf8, "Coins: 1 / 8");
    const glm::vec3 cameraAfter = level.registry().get<TransformComponent>(camera).position;
    EXPECT_LT(cameraAfter.x, cameraBefore.x - 1.0f) << "the camera followed the player to the left";
}

TEST_F(ScriptTest, LoadSceneSwitchesLevels) {
    const std::filesystem::path project = dir.path();
    writeScript(project, "Levels.cpp", R"cpp(#include <lumora/Script.hpp>
using namespace lumora::script;
// Level 1: after 0.1 s go to level 2 (and reject paths outside the project).
class Exit : public Behaviour {
public:
    void update(float) override {
        if (!asked_ && time() >= 0.1) {
            asked_ = true;
            if (loadScene("../outside.lscene")) warn("accepted a path outside the project");
            if (!loadScene("Content/Scenes/Level2.lscene")) warn("loadScene refused level 2");
        }
    }
private:
    bool asked_ = false;
};
LUMORA_BEHAVIOUR(Exit);
// Level 2: counts its own time from 0.
class Clock : public Behaviour {
public:
    void start() override { setPosition(Vec3{static_cast<float>(time()), 0.0f, 0.0f}); }
    void update(float dt) override { setPosition(position() + Vec3{0.0f, dt, 0.0f}); }
};
LUMORA_BEHAVIOUR(Clock);
)cpp");
    {
        Scene level2("Level 2");
        level2.registry().emplace<ScriptComponent>(level2.createEntity("Clock")).behaviour = "Clock";
        (void)level2.createEntity("Scenery");
        auto& c = level2.registry().emplace<ColliderComponent>(level2.createEntity("Floor"));
        c.size = {10.0f, 1.0f, 10.0f};
        ASSERT_TRUE(SceneSerializer::save(level2, project / "Content" / "Scenes" / "Level2.lscene").has_value());
    }
    app::ScriptBuilder builder(QString::fromStdString(project.string()));
    QString log;
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();
    auto module = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(module.has_value()) << module.error().message;

    Scene scene("Level 1");
    scene.registry().emplace<ScriptComponent>(scene.createEntity("Door")).behaviour = "Exit";
    Simulation sim(scene);
    sim.setProjectRoot(project);
    sim.setScripts(*module, nullptr);
    EXPECT_EQ(sim.physics(), nullptr) << "level 1 has no colliders";
    for (int i = 0; i < 6; ++i) sim.step();  // t = 0.1 s at the end of step 6
    EXPECT_EQ(sim.sceneLoads(), 0u);
    sim.step();
    ASSERT_EQ(sim.sceneLoads(), 1u);
    EXPECT_EQ(sim.loadedScene().generic_string(), "Content/Scenes/Level2.lscene");
    EXPECT_EQ(scene.name(), "Level 2");
    EXPECT_EQ(scene.entityCount(), 3u);
    EXPECT_EQ(sim.stepCount(), 0u) << "time starts over";
    ASSERT_NE(sim.physics(), nullptr) << "level 2's colliders get a physics world";
    EXPECT_EQ(sim.physics()->bodyCount(), 1u);
    for (int i = 0; i < 60; ++i) sim.step();
    EXPECT_EQ(sim.scriptInstances(), 1u) << "only level 2's behaviour runs";
    entt::entity clock = entt::null;
    for (auto [e, name] : scene.registry().view<NameComponent>().each()) {
        if (name.name == "Clock") clock = e;
    }
    ASSERT_TRUE(clock != entt::null);
    const glm::vec3 p = scene.registry().get<TransformComponent>(clock).position;
    EXPECT_NEAR(p.x, 0.0f, 1e-6f) << "started at time 0";
    EXPECT_NEAR(p.y, 1.0f, 1e-3f) << "ran for a second";
}

#include "input/InputState.hpp"

TEST_F(ScriptTest, MenusReactToClicksAndKeys) {
    const std::filesystem::path project = dir.path();
    writeScript(project, "Menu.cpp", R"cpp(#include <lumora/Script.hpp>
using namespace lumora::script;
// A pause menu: Escape opens it, "Resume" closes it, the slider sets the height.
class Menu : public Behaviour {
public:
    void update(float) override {
        if (keyPressed(Key::Escape)) open_ = !open_;
        if (!open_) return;
        panel(80, 80, 360, 260);
        if (button("Resume", 100, 100, 200, 50)) open_ = false;
        slider("Height", height_, 0.0f, 10.0f, 100, 200, 200, 40);
        setPosition(Vec3{static_cast<float>(clicks_), height_, open_ ? 1.0f : 0.0f});
        if (mousePressed(MouseButton::Left)) ++clicks_;
    }
private:
    bool open_ = false;
    float height_ = 0.0f;
    int clicks_ = 0;
};
LUMORA_BEHAVIOUR(Menu);
)cpp");
    app::ScriptBuilder builder(QString::fromStdString(project.string()));
    QString log;
    ASSERT_TRUE(builder.buildBlocking(&log)) << log.toStdString();
    auto module = ScriptModule::load(builder.libraryPath().toStdString(), builder.loadDir().toStdString());
    ASSERT_TRUE(module.has_value()) << module.error().message;

    Scene scene("Menu");
    const entt::entity e = scene.createEntity("UI");
    scene.registry().emplace<ScriptComponent>(e).behaviour = "Menu";
    InputState input;
    Simulation sim(scene);
    sim.setScripts(*module, &input);
    auto position = [&] { return scene.registry().get<TransformComponent>(e).position; };
    sim.step();
    EXPECT_TRUE(sim.hud().empty()) << "closed";

    input.setKey(Key::Escape, true);
    sim.step();
    EXPECT_FALSE(sim.hud().empty()) << "Escape opened the menu";
    EXPECT_FLOAT_EQ(position().z, 1.0f);

    // Drag the slider to 3/4: height 7.5.
    input.setMousePosition({250.0f, 220.0f});
    input.setMouseButton(MouseButton::Left, true);
    sim.step();
    input.setMouseButton(MouseButton::Left, false);
    sim.step();
    EXPECT_NEAR(position().y, 7.5f, 1e-4f);
    EXPECT_FLOAT_EQ(position().x, 1.0f) << "one press";

    // Click Resume (a click shorter than a step): the menu closes.
    input.setMousePosition({200.0f, 125.0f});
    input.setMouseButton(MouseButton::Left, true);
    input.setMouseButton(MouseButton::Left, false);
    sim.step();
    sim.step();
    EXPECT_TRUE(sim.hud().empty()) << "Resume closed it";
}
