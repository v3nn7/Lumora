// The visual script editor: building a graph through the panel, saving it, and running what
// was saved; the Content Browser's starter files.
#include "TestUtils.hpp"

#include "editor/ContentBrowserPanel.hpp"
#include "editor/GraphEditorPanel.hpp"
#include "runtime/Simulation.hpp"
#include "runtime/VisualScript.hpp"
#include "scene/Scene.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMenu>

#include <gtest/gtest.h>

#include <memory>

using namespace lumora;
using editor::GraphEditorPanel;
using visual::PinType;

TEST(GraphEditor, BuildSaveAndRunAGraph) {
    test::TempDir dir;
    const QString root = QString::fromStdString(dir.path().string());
    QDir(root).mkpath(QStringLiteral("Content"));
    const QString file = root + QStringLiteral("/Content/Spin.lgraph");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"({"version": 1, "nodes": [], "links": []})");
    }
    GraphEditorPanel panel(root);
    ASSERT_TRUE(panel.open(file));
    EXPECT_FALSE(panel.isDirty());

    panel.addVariable(QStringLiteral("height"), QStringLiteral("number"));
    const int start = panel.addNode("event.start", {0, 0});
    const int set = panel.addNode("entity.setPosition", {300, 0});
    const int make = panel.addNode("vec.make", {100, 150});
    const int add = panel.addNode("math.add", {-100, 150});
    ASSERT_GT(start * set * make * add, 0);
    EXPECT_EQ(panel.addNode("no.such", {0, 0}), 0);
    EXPECT_TRUE(panel.isDirty());

    EXPECT_TRUE(panel.connectPins(start, "exec", set, "in"));
    EXPECT_TRUE(panel.connectPins(make, "vector", set, "value"));
    EXPECT_TRUE(panel.connectPins(add, "result", make, "y"));
    EXPECT_FALSE(panel.connectPins(start, "exec", make, "x")) << "exec cannot feed a number";
    EXPECT_FALSE(panel.connectPins(make, "vector", add, "a")) << "a vector does not become a number";
    panel.setLiteral(add, "a", 2.0);
    panel.setLiteral(add, "b", 3.0);
    EXPECT_EQ(panel.linkCount(), 3u);
    EXPECT_TRUE(panel.connectPins(add, "result", make, "x"));
    EXPECT_TRUE(panel.connectPins(add, "result", make, "x")) << "linking twice is a no-op";
    EXPECT_EQ(panel.linkCount(), 4u);
    EXPECT_TRUE(panel.disconnectInput(make, "x"));
    EXPECT_EQ(panel.linkCount(), 3u);
    ASSERT_TRUE(panel.save());
    EXPECT_FALSE(panel.isDirty());

    // What was saved parses and runs.
    std::vector<std::string> warnings;
    auto graph = visual::Graph::load(dir.path() / "Content/Spin.lgraph", &warnings);
    ASSERT_TRUE(graph.has_value()) << graph.error().message;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ((*graph)->variables().size(), 1u);
    Scene scene;
    const entt::entity e = scene.createEntity("Box");
    scene.registry().emplace<ScriptComponent>(e).behaviour = "Content/Spin.lgraph";
    Simulation sim(scene);
    sim.setProjectRoot(dir.path());
    sim.setScripts(nullptr, nullptr);
    sim.step();
    EXPECT_FLOAT_EQ(scene.registry().get<TransformComponent>(e).position.y, 5.0f);

    // Reopening restores nodes and links; removing a node drops its links.
    GraphEditorPanel again(root);
    ASSERT_TRUE(again.open(file));
    EXPECT_EQ(again.nodeCount(), 4u);
    EXPECT_EQ(again.linkCount(), 3u);
    EXPECT_EQ(again.variableNames(), QStringList{QStringLiteral("height")});
    EXPECT_TRUE(again.removeNode(make));
    EXPECT_EQ(again.linkCount(), 1u);
}

TEST(GraphEditor, PinCompatibility) {
    auto ok = [](PinType a, PinType b) { return GraphEditorPanel::compatible(static_cast<int>(a), static_cast<int>(b)); };
    EXPECT_TRUE(ok(PinType::Exec, PinType::Exec));
    EXPECT_FALSE(ok(PinType::Exec, PinType::Number));
    EXPECT_TRUE(ok(PinType::Number, PinType::Bool));
    EXPECT_TRUE(ok(PinType::Number, PinType::Vec3));
    EXPECT_TRUE(ok(PinType::Entity, PinType::String));
    EXPECT_TRUE(ok(PinType::Any, PinType::Entity));
    EXPECT_FALSE(ok(PinType::Vec3, PinType::Entity));
}

TEST(GraphEditor, ContentBrowserStarterScriptsRun) {
    test::TempDir dir;
    const QString root = QString::fromStdString(dir.path().string());
    editor::ContentBrowserPanel browser(root);
    const QString lua = browser.createLuaScript(QStringLiteral("Walker"));
    const QString graphFile = browser.createVisualScript(QStringLiteral("Spinner"));
    ASSERT_FALSE(lua.isEmpty());
    ASSERT_FALSE(graphFile.isEmpty());
    EXPECT_TRUE(browser.createLuaScript(QStringLiteral("Walker")).isEmpty()) << "never overwrites";

    Scene scene;
    const entt::entity walker = scene.createEntity("Walker");
    scene.registry().emplace<ScriptComponent>(walker).behaviour = QDir(root).relativeFilePath(lua).toStdString();
    const entt::entity spinner = scene.createEntity("Spinner");
    scene.registry().emplace<ScriptComponent>(spinner).behaviour = QDir(root).relativeFilePath(graphFile).toStdString();
    Simulation sim(scene);
    sim.setProjectRoot(dir.path());
    sim.setScripts(nullptr, nullptr);
    for (int i = 0; i < 10; ++i) sim.step();
    EXPECT_EQ(sim.scriptInstances(), 2u) << "both starter scripts load and run without errors";
    EXPECT_GT(scene.registry().get<TransformComponent>(spinner).rotationDeg.y, 1.0f) << "the starter graph spins";
}

namespace {
/// Triggers the right-click menu entry `text` at a scene position; false if there is none.
bool rightClick(GraphEditorPanel& panel, QPointF at, const QString& text) {
    std::unique_ptr<QMenu> menu{panel.contextMenuAt(at, nullptr)};
    if (!menu) return false;
    for (QAction* action : menu->actions()) {
        if (action->text().startsWith(text) && action->isEnabled()) {
            action->trigger();
            return true;
        }
    }
    return false;
}
} // namespace

TEST(GraphEditor, RightClickDeletesNodesAndCutsLinks) {
    test::TempDir dir;
    const QString root = QString::fromStdString(dir.path().string());
    editor::ContentBrowserPanel browser(root);
    GraphEditorPanel panel(root);
    ASSERT_TRUE(panel.open(browser.createVisualScript(QStringLiteral("Cut"))));
    const std::size_t starterNodes = panel.nodeCount();
    const int start = panel.addNode("event.start", {0, 1000});
    const int set = panel.addNode("entity.setPosition", {500, 1000});
    const int make = panel.addNode("vec.make", {500, 1300});
    ASSERT_TRUE(panel.connectPins(start, "exec", set, "in"));
    ASSERT_TRUE(panel.connectPins(make, "vector", set, "value"));
    const std::size_t links = panel.linkCount();

    EXPECT_EQ(panel.contextMenuAt({-5000, -5000}, nullptr), nullptr) << "empty canvas: the node list instead";

    // A pin: Start's exec output sits on the node's right edge, on the first row.
    const QPointF startExec(300, 1000 + 26 + 13);
    EXPECT_TRUE(rightClick(panel, startExec, QStringLiteral("Break Links")));
    EXPECT_EQ(panel.linkCount(), links - 1);
    EXPECT_FALSE(rightClick(panel, startExec, QStringLiteral("Break Links"))) << "nothing left to break";

    // A link: the middle of its curve.
    auto* scene = panel.findChild<QGraphicsView*>()->scene();
    QPointF linkMiddle;
    for (QGraphicsItem* item : scene->items()) {
        auto* path = dynamic_cast<QGraphicsPathItem*>(item);
        if (path != nullptr && item->parentItem() == nullptr && path->path().pointAtPercent(1.0).x() > 400 &&
            path->path().pointAtPercent(1.0).y() > 1000) {
            linkMiddle = path->path().pointAtPercent(0.5);
        }
    }
    ASSERT_FALSE(linkMiddle.isNull());
    EXPECT_TRUE(rightClick(panel, linkMiddle, QStringLiteral("Remove Link")));
    EXPECT_EQ(panel.linkCount(), links - 2);

    // A node: duplicate it, link it, break its links, delete it.
    const QPointF onMake(500 + 150, 1300 + 10);
    EXPECT_TRUE(rightClick(panel, onMake, QStringLiteral("Duplicate")));
    EXPECT_EQ(panel.nodeCount(), starterNodes + 4);
    ASSERT_TRUE(panel.connectPins(make, "vector", set, "value"));
    EXPECT_TRUE(rightClick(panel, onMake, QStringLiteral("Break All Links")));
    EXPECT_EQ(panel.linkCount(), links - 2);
    EXPECT_TRUE(rightClick(panel, onMake, QStringLiteral("Delete Node")));
    EXPECT_EQ(panel.nodeCount(), starterNodes + 3);
    EXPECT_TRUE(panel.isDirty());
}

TEST(GraphEditor, Snapshot) {
    // Visual check only: LUMORA_TEST_SNAPSHOT_DIR=... writes a picture of the panel.
    const QString out = qEnvironmentVariable("LUMORA_TEST_SNAPSHOT_DIR");
    if (out.isEmpty()) GTEST_SKIP() << "set LUMORA_TEST_SNAPSHOT_DIR to write the picture";
    test::TempDir dir;
    const QString root = QString::fromStdString(dir.path().string());
    editor::ContentBrowserPanel browser(root);
    const QString file = browser.createVisualScript(QStringLiteral("Spinner"));
    GraphEditorPanel panel(root);
    ASSERT_TRUE(panel.open(file));
    panel.resize(1400, 700);
    panel.show();
    QCoreApplication::processEvents();
    panel.fitGraph();
    QCoreApplication::processEvents();
    panel.grab().save(out + QStringLiteral("/graph-editor.png"));
}
