// Gameplay scripting end to end: real C++ scripts compiled with CMake and the system compiler,
// loaded into a simulation, changed, rebuilt and hot-reloaded while it runs.
#include "TestUtils.hpp"

#include "common/ScriptBuilder.hpp"
#include "runtime/Scripting.hpp"
#include "runtime/Simulation.hpp"
#include "scene/Scene.hpp"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <gtest/gtest.h>

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
