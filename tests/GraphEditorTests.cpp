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
#include <QKeyEvent>
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

namespace {
/// A panel with the starter graph open (ids 1-7, see ContentBrowserPanel::createVisualScript).
struct StarterGraph {
    test::TempDir dir;
    QString root = QString::fromStdString(dir.path().generic_string());
    editor::ContentBrowserPanel browser{root};
    QString file = browser.createVisualScript(QStringLiteral("Spinner"));
    GraphEditorPanel panel{root};
    StarterGraph() { EXPECT_TRUE(panel.open(file)); }
};

void press(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent down(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(target, &down);
}
} // namespace

TEST(GraphEditor, UndoAndRedoEveryEdit) {
    StarterGraph g;
    GraphEditorPanel& panel = g.panel;
    const auto original = panel.document();
    EXPECT_FALSE(panel.canUndo());

    const int added = panel.addNode("math.add", {0, 600});
    panel.undo();  // the pending edit becomes a step and is undone
    EXPECT_EQ(panel.document(), original);
    EXPECT_FALSE(panel.isDirty()) << "back to what is on disk";
    EXPECT_TRUE(panel.canRedo());
    panel.redo();
    EXPECT_EQ(panel.nodeCount(), 8u);
    EXPECT_TRUE(panel.isDirty());

    ASSERT_TRUE(panel.removeNode(4));  // takes its links along
    EXPECT_EQ(panel.linkCount(), 4u);
    panel.undo();
    EXPECT_EQ(panel.nodeCount(), 8u);
    EXPECT_EQ(panel.linkCount(), 6u) << "the node comes back with its links";
    panel.undo();
    EXPECT_EQ(panel.document(), original);

    // Ctrl+Z on the canvas reaches the graph (not the editor's scene undo).
    panel.redo();
    auto* view = panel.findChild<QGraphicsView*>();
    press(view, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(panel.document(), original);
    press(view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(panel.nodeCount(), 8u);
    EXPECT_GT(added, 7);
}

TEST(GraphEditor, CopyPasteAndDuplicateKeepInnerLinks) {
    StarterGraph g;
    GraphEditorPanel& panel = g.panel;
    panel.selectNodes({5, 6, 7});  // vec.make <- mul <- get speed: two links inside, two out
    panel.copySelection();
    ASSERT_TRUE(panel.paste(QPointF(2000, 2000)));
    EXPECT_EQ(panel.nodeCount(), 10u);
    EXPECT_EQ(panel.linkCount(), 8u) << "the two links between the copies, none to the rest";
    const auto pasted = panel.selectedNodes();
    ASSERT_EQ(pasted.size(), 3u) << "the copies are selected";
    EXPECT_GT(pasted.front(), 7) << "with new ids";
    const auto doc = panel.document();
    for (const auto& n : doc["nodes"]) {
        if (n["id"] == pasted.front()) {
            EXPECT_GE(n["x"].get<double>(), 2000.0);
            EXPECT_GE(n["y"].get<double>(), 2000.0);
        }
    }
    panel.duplicateSelection();
    EXPECT_EQ(panel.nodeCount(), 13u);
    EXPECT_EQ(panel.linkCount(), 10u);

    panel.selectNodes({1});
    panel.cutSelection();
    EXPECT_EQ(panel.nodeCount(), 12u);
    EXPECT_TRUE(panel.paste());
    EXPECT_EQ(panel.nodeCount(), 13u);
}

TEST(GraphEditor, CommentsAreSavedAndCarryTheirNodes) {
    StarterGraph g;
    GraphEditorPanel& panel = g.panel;
    panel.selectNodes({1, 2});
    panel.commentSelection();
    ASSERT_EQ(panel.commentCount(), 1u);
    ASSERT_TRUE(panel.save());
    auto doc = panel.document();
    ASSERT_EQ(doc["comments"].size(), 1u);
    EXPECT_EQ(doc["comments"][0]["text"], "Comment");
    const double x1 = doc["nodes"][0]["x"].get<double>();

    // What was saved still runs (the runtime ignores comments) and opens again.
    EXPECT_TRUE(visual::Graph::parse(doc).has_value());
    GraphEditorPanel reopened(g.root);
    ASSERT_TRUE(reopened.open(g.file));
    EXPECT_EQ(reopened.commentCount(), 1u);

    // Dragging the comment's title moves the nodes inside it.
    auto* view = panel.findChild<QGraphicsView*>();
    QGraphicsItem* comment = nullptr;
    for (QGraphicsItem* item : view->scene()->items()) {
        if (item->zValue() == -3) comment = item;
    }
    ASSERT_NE(comment, nullptr);
    view->resize(800, 600);
    view->show();
    view->centerOn(comment->sceneBoundingRect().topLeft());
    const QPoint grab = view->mapFromScene(comment->sceneBoundingRect().topLeft() + QPointF(60, 12));
    auto mouse = [&](QEvent::Type type, QPoint at, Qt::MouseButtons buttons) {
        QMouseEvent e(type, at, view->viewport()->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &e);
    };
    view->scene()->clearSelection();
    mouse(QEvent::MouseButtonPress, grab, Qt::LeftButton);
    mouse(QEvent::MouseMove, grab + QPoint(100, 0), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, grab + QPoint(100, 0), Qt::NoButton);
    doc = panel.document();
    EXPECT_NEAR(doc["nodes"][0]["x"].get<double>() - x1, 100.0 / view->transform().m11(), 2.0);
}

TEST(GraphEditor, ReroutePointsKeepTheGraphWorking) {
    StarterGraph g;
    GraphEditorPanel& panel = g.panel;
    const int exec = panel.insertReroute(3, "exec", 4, "in", {600, 160});
    const int data = panel.insertReroute(5, "vector", 4, "degrees", {900, 300});
    ASSERT_GT(exec, 0);
    ASSERT_GT(data, 0);
    EXPECT_EQ(panel.nodeCount(), 9u);
    EXPECT_EQ(panel.linkCount(), 8u) << "each split link becomes two";
    ASSERT_TRUE(panel.save());

    // The spinner still spins through both dots.
    Scene scene;
    const entt::entity spinner = scene.createEntity("Spinner");
    scene.registry().emplace<ScriptComponent>(spinner).behaviour = QDir(g.root).relativeFilePath(g.file).toStdString();
    Simulation sim(scene);
    sim.setProjectRoot(g.dir.path());
    sim.setScripts(nullptr, nullptr);
    for (int i = 0; i < 10; ++i) sim.step();
    EXPECT_GT(scene.registry().get<TransformComponent>(spinner).rotationDeg.y, 1.0f);
}

TEST(GraphEditor, VariablesDropInAsGetAndSetNodes) {
    StarterGraph g;
    GraphEditorPanel& panel = g.panel;
    const int get = panel.addVariableNode(QStringLiteral("speed"), false, {0, 700});
    const int set = panel.addVariableNode(QStringLiteral("speed"), true, {300, 700});
    const auto doc = panel.document();
    int found = 0;
    for (const auto& n : doc["nodes"]) {
        if (n["id"] != get && n["id"] != set) continue;
        EXPECT_EQ(n["type"], n["id"] == get ? "var.get" : "var.set");
        EXPECT_EQ(n["inputs"]["name"], "speed");
        ++found;
    }
    EXPECT_EQ(found, 2);
}

TEST(GraphEditor, LiveViewLightsWhatRunsWhilePlaying) {
    StarterGraph g;
    GraphEditorPanel& panel = g.panel;
    panel.refreshLive();
    EXPECT_FALSE(panel.isLive()) << "nothing plays yet";

    visual::trace::clear();
    visual::trace::setEnabled(true);
    Scene scene;
    const entt::entity spinner = scene.createEntity("Spinner");
    scene.registry().emplace<ScriptComponent>(spinner).behaviour = QDir(g.root).relativeFilePath(g.file).toStdString();
    {
        Simulation sim(scene);
        sim.setProjectRoot(g.dir.path());
        sim.setScripts(nullptr, nullptr);
        for (int i = 0; i < 5; ++i) sim.step();
        panel.refreshLive();
        EXPECT_TRUE(panel.isLive());
        EXPECT_GT(panel.nodeGlow(3), 0.9f) << "On Update fires every step";
        EXPECT_FALSE(panel.liveValue(3, "dt").isEmpty()) << "with its delta time";
        EXPECT_GT(panel.nodeGlow(4), 0.9f) << "Rotate By runs every step";
        EXPECT_GT(panel.nodeGlow(6), 0.9f) << "the multiply feeding it was read this step";
        EXPECT_LT(panel.nodeGlow(2), 0.95f) << "Log ran once, at the start";
        EXPECT_EQ(panel.liveValue(7, "value"), QStringLiteral("90")) << "the speed variable";
        EXPECT_FALSE(panel.liveValue(6, "result").isEmpty());
    }
    visual::trace::setEnabled(false);
    visual::trace::clear();
    panel.refreshLive();
    EXPECT_FALSE(panel.isLive()) << "stopping clears the view";
    EXPECT_TRUE(panel.liveValue(7, "value").isEmpty());
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

    // The same graph with a comment, a reroute dot and the live view of a running game.
    panel.selectNodes({1, 2});
    panel.commentSelection();
    panel.insertReroute(3, "exec", 4, "in", {700, 175});
    ASSERT_TRUE(panel.save());
    visual::trace::clear();
    visual::trace::setEnabled(true);
    Scene scene;
    const entt::entity spinner = scene.createEntity("Spinner");
    scene.registry().emplace<ScriptComponent>(spinner).behaviour = QDir(root).relativeFilePath(file).toStdString();
    Simulation sim(scene);
    sim.setProjectRoot(dir.path());
    sim.setScripts(nullptr, nullptr);
    for (int i = 0; i < 5; ++i) sim.step();
    panel.refreshLive();
    panel.fitGraph();
    QCoreApplication::processEvents();
    panel.grab().save(out + QStringLiteral("/graph-editor-live.png"));
    visual::trace::setEnabled(false);
    visual::trace::clear();
}

#include "assets/Material.hpp"
#include "assets/MaterialGraph.hpp"

#include <QElapsedTimer>
#include <QLabel>
#include <QThread>

#include <nlohmann/json.hpp>

TEST(GraphEditor, MaterialGraphsBuildSaveThemselvesAndCompile) {
    test::TempDir dir;
    const QString root = QString::fromStdString(dir.path().string());
    QDir(root).mkpath(QStringLiteral("Content/Materials"));
    const QString file = root + QStringLiteral("/Content/Materials/Rust.lmgraph");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(defaultMaterialGraph().dump()));
    }
    GraphEditorPanel panel(root, editor::GraphKind::Material);
    ASSERT_TRUE(panel.open(file));
    EXPECT_EQ(panel.nodeCount(), 1u) << "the Material Output";
    EXPECT_TRUE(panel.compileError().isEmpty());
    EXPECT_EQ(panel.addNode("event.start", {0, 0}), 0) << "visual script nodes are not material nodes";
    const int texture = panel.addNode("texture.color", {-500, 0});
    const int tint = panel.addNode("const.color", {-500, 200});
    const int multiply = panel.addNode("math.multiply", {-250, 0});
    const int noise = panel.addNode("noise.value", {-250, 250});
    ASSERT_GT(texture * tint * multiply * noise, 0);
    const int output = 1;
    panel.setLiteral(texture, "texture", "Content/Textures/rust.png");
    panel.setLiteral(tint, "color", nlohmann::json::array({0.8, 0.4, 0.2}));
    EXPECT_TRUE(panel.connectPins(texture, "rgb", multiply, "a"));
    EXPECT_TRUE(panel.connectPins(tint, "color", multiply, "b"));
    EXPECT_TRUE(panel.connectPins(multiply, "result", output, "baseColor"));
    EXPECT_TRUE(panel.connectPins(noise, "value", output, "roughness"));
    EXPECT_TRUE(panel.connectPins(multiply, "result", output, "metallic")) << "a vector into a number takes its first component";
    EXPECT_TRUE(panel.isDirty());

    // Saved by itself a moment later, compiled, and readable as a material.
    auto waitSaved = [&] {
        QElapsedTimer timer;
        timer.start();
        while (panel.isDirty() && timer.elapsed() < 3000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
    };
    waitSaved();
    EXPECT_FALSE(panel.isDirty());
    EXPECT_TRUE(panel.compileError().isEmpty()) << panel.compileError().toStdString();
    auto material = loadMaterial(std::filesystem::path(file.toStdU16String()));
    ASSERT_TRUE(material.has_value()) << material.error().message;
    ASSERT_NE(material->program, nullptr);
    ASSERT_EQ(material->program->textures.size(), 1u);
    EXPECT_EQ(material->program->textures[0].key, "Content/Textures/rust.png");
    auto* status = panel.findChild<QLabel*>(QStringLiteral("materialGraphStatus"));
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("1 texture"))) << status->text().toStdString();

    // A second Material Output does not compile: the panel says so.
    panel.addNode("output.material", {300, 300});
    waitSaved();
    EXPECT_FALSE(panel.compileError().isEmpty());
    EXPECT_TRUE(status->text().startsWith(QStringLiteral("Does not compile"))) << status->text().toStdString();
    panel.undo();
    waitSaved();
    EXPECT_TRUE(panel.compileError().isEmpty());
}
