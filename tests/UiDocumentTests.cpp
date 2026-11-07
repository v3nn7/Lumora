#include "TestUtils.hpp"

#include "input/InputState.hpp"
#include "runtime/GameUi.hpp"
#include "runtime/Simulation.hpp"
#include "runtime/UiDocument.hpp"
#include "runtime/UiScreens.hpp"
#include "scene/Hud.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>

using namespace lumora;

namespace {

UiWidget widget(UiWidget::Type type, std::string name, glm::vec2 size = {0.0f, 40.0f}) {
    UiWidget w;
    w.type = type;
    w.name = std::move(name);
    w.text = w.name;
    w.size = size;
    return w;
}

/// A centred 300 x 300 menu: a column with a title, Play, a Music slider and a scrolled list
/// of ten rows.
UiDocument menuDocument() {
    UiDocument doc = defaultUiDocument();
    UiWidget menu = widget(UiWidget::Type::Panel, "Menu");
    menu.anchorMin = menu.anchorMax = {0.5f, 0.5f};
    menu.offsetMin = {-150.0f, -150.0f};
    menu.offsetMax = {150.0f, 150.0f};
    menu.layout = UiWidget::Layout::Vertical;
    menu.padding = glm::vec4(10.0f);
    menu.spacing = 10.0f;
    menu.color = {0.1f, 0.1f, 0.1f, 0.9f};
    menu.children.push_back(widget(UiWidget::Type::Label, "Title"));
    menu.children.push_back(widget(UiWidget::Type::Button, "Play"));
    UiWidget music = widget(UiWidget::Type::Slider, "Music");
    music.value = 0.5f;
    menu.children.push_back(music);
    UiWidget list = widget(UiWidget::Type::Panel, "List", {0.0f, 100.0f});
    list.layout = UiWidget::Layout::Vertical;
    list.scroll = true;
    list.spacing = 0.0f;
    list.color = glm::vec4(0.0f);
    for (int i = 0; i < 10; ++i) list.children.push_back(widget(UiWidget::Type::Label, "Row" + std::to_string(i), {0.0f, 30.0f}));
    menu.children.push_back(list);
    doc.root.children.push_back(menu);
    return doc;
}

const UiPlaced* placedNamed(const std::vector<UiPlaced>& placed, std::string_view name) {
    for (const UiPlaced& p : placed) {
        if (p.widget->name == name) return &p;
    }
    return nullptr;
}

} // namespace

TEST(UiDocument, SavesLoadsAndRejectsBrokenFiles) {
    test::TempDir dir;
    const UiDocument doc = menuDocument();
    ASSERT_TRUE(saveUiDocument(doc, dir.path() / "Content" / "UI" / "Menu.lui").has_value());
    auto loaded = loadUiDocument(dir.path() / "Content" / "UI" / "Menu.lui");
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(uiToJson(*loaded), uiToJson(doc)) << "everything survives";
    const UiWidget* music = findUiWidget(loaded->root, "Music");
    ASSERT_NE(music, nullptr);
    EXPECT_EQ(music->type, UiWidget::Type::Slider);
    EXPECT_FLOAT_EQ(music->value, 0.5f);
    EXPECT_FALSE(uiFromJson(nlohmann::json{{"root", {{"type", "spinner"}}}}).has_value()) << "unknown type";
    EXPECT_FALSE(uiFromJson(nlohmann::json{{"root", {{"type", "button"}}}}).has_value()) << "the root is a panel";
    EXPECT_FALSE(uiFromJson(nlohmann::json{{"root", {{"layout", "spiral"}}}}).has_value());
    EXPECT_FALSE(uiFromJson(nlohmann::json{{"root", {{"anchorMin", {1, 2, 3}}}}}).has_value());
    auto minimal = uiFromJson(nlohmann::json{{"root", {{"children", {{{"type", "label"}, {"text", "Hi"}}}}}}});
    ASSERT_TRUE(minimal.has_value()) << minimal.error().message;
    EXPECT_EQ(minimal->root.children[0].text, "Hi") << "missing fields keep their defaults";
}

TEST(UiDocument, AnchorsStacksGridsAndScrolling) {
    UiDocument doc = menuDocument();
    std::map<std::string, float> heights;
    auto placed = layoutUi(doc, {800.0f, 600.0f}, {}, &heights);
    const UiPlaced* menu = placedNamed(placed, "Menu");
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(menu->rect, glm::vec4(250.0f, 150.0f, 300.0f, 300.0f)) << "pinned to the centre";
    const UiPlaced* title = placedNamed(placed, "Title");
    const UiPlaced* play = placedNamed(placed, "Play");
    ASSERT_TRUE(title && play);
    EXPECT_EQ(title->rect, glm::vec4(260.0f, 160.0f, 280.0f, 40.0f)) << "inside the padding, full width";
    EXPECT_FLOAT_EQ(play->rect.y, 210.0f) << "below, after the spacing";
    EXPECT_EQ(menu->path, "0");
    EXPECT_EQ(play->path, "0/1");
    EXPECT_EQ(uiWidgetAt(doc.root, "0/1"), &doc.root.children[0].children[1]);
    // The list: ten 30 px rows in 100 px, cut to it; scrolling moves them up.
    const UiPlaced* row0 = placedNamed(placed, "Row0");
    ASSERT_NE(row0, nullptr);
    EXPECT_FLOAT_EQ(heights["0/3"], 300.0f);
    const UiPlaced* list = placedNamed(placed, "List");
    EXPECT_EQ(row0->clip, glm::vec4(list->rect.x, list->rect.y, list->rect.x + list->rect.z, list->rect.y + list->rect.w));
    auto scrolled = layoutUi(doc, {800.0f, 600.0f}, {{"0/3", 60.0f}});
    EXPECT_FLOAT_EQ(placedNamed(scrolled, "Row0")->rect.y, row0->rect.y - 60.0f);
    // Hidden widgets (and what they hold) are left out; a grid fills rows of columns.
    doc.root.children[0].children[3].visible = false;
    EXPECT_EQ(placedNamed(layoutUi(doc, {800.0f, 600.0f}), "Row0"), nullptr);
    UiDocument grid = defaultUiDocument();
    UiWidget g = widget(UiWidget::Type::Panel, "Grid");
    g.anchorMax = {1.0f, 1.0f};
    g.offsetMax = {0.0f, 0.0f};
    g.layout = UiWidget::Layout::Grid;
    g.columns = 3;
    g.spacing = 0.0f;
    for (int i = 0; i < 5; ++i) g.children.push_back(widget(UiWidget::Type::Image, "Cell" + std::to_string(i), {0.0f, 50.0f}));
    grid.root.children.push_back(g);
    auto cells = layoutUi(grid, {300.0f, 300.0f});
    EXPECT_EQ(placedNamed(cells, "Cell4")->rect, glm::vec4(100.0f, 50.0f, 100.0f, 50.0f)) << "second row, second column";
}

TEST(UiDocument, OpenedByScriptsClickedChangedAndRetold) {
    test::TempDir dir;
    ASSERT_TRUE(saveUiDocument(menuDocument(), dir.path() / "Content" / "UI" / "Menu.lui").has_value());
    std::filesystem::create_directories(dir.path() / "Scripts");
    std::ofstream(dir.path() / "Scripts" / "Menu.lua") << R"(
        local M = Behaviour:extend()
        function M:start()
            self.ok = self:openUi("Content/UI/Menu.lui")
            self.clicks = 0
        end
        function M:update()
            if self:uiClicked("Play") then
                self.clicks = self.clicks + 1
                self:setUiText("Title", "Clicked " .. self.clicks)
            end
            if self:uiChanged("Music") then self:setUiText("Row0", "Volume " .. string.format("%.1f", self:uiValue("Music"))) end
        end
        return M
    )";
    Scene scene("Ui");
    const entt::entity e = scene.createEntity("Menu");
    scene.registry().emplace<ScriptComponent>(e).behaviour = "Scripts/Menu.lua";
    InputState input;
    Simulation sim(scene);
    sim.setProjectRoot(dir.path());
    sim.setScripts(nullptr, &input);
    sim.hud().screenSize = {800.0f, 600.0f};
    sim.step();  // opens the document (and draws it next step)
    sim.step();
    auto textOf = [&](std::string_view prefix) -> std::string {
        for (const HudCanvas::Text& t : sim.hud().texts) {
            if (t.utf8.starts_with(prefix)) return t.utf8;
        }
        return {};
    };
    EXPECT_EQ(textOf("Title"), "Title");
    EXPECT_EQ(textOf("Play"), "Play");
    // Click Play (y 210..250, x 260..540).
    input.setMousePosition({400.0f, 230.0f});
    input.setMouseButton(MouseButton::Left, true);
    sim.step();
    input.setMouseButton(MouseButton::Left, false);
    sim.step();
    sim.step();
    EXPECT_EQ(textOf("Clicked"), "Clicked 1");
    // Drag the slider (y 260..300) to its right end.
    input.setMousePosition({300.0f, 280.0f});
    input.setMouseButton(MouseButton::Left, true);
    sim.step();
    input.setMousePosition({600.0f, 280.0f});
    sim.step();
    input.setMouseButton(MouseButton::Left, false);
    sim.step();
    sim.step();
    EXPECT_EQ(textOf("Volume"), "Volume 1.0");
    // The list's rows are cut to it: the last ones are clipped away entirely, the first not.
    bool clippedRow = false;
    for (const HudCanvas::Text& t : sim.hud().texts) {
        if (t.utf8 == "Row1") clippedRow = t.clip != HudCanvas::kNoClip;
    }
    EXPECT_TRUE(clippedRow);
    EXPECT_EQ(textOf("Row9"), "") << "scrolled out of sight: not drawn";
}

TEST(UiDocument, ScreensOpenInOrderAndCloseByKey) {
    test::TempDir dir;
    ASSERT_TRUE(saveUiDocument(menuDocument(), dir.path() / "A.lui").has_value());
    UiDocument other = defaultUiDocument();
    other.root.children.push_back(widget(UiWidget::Type::Label, "Play"));
    ASSERT_TRUE(saveUiDocument(other, dir.path() / "B.lui").has_value());
    UiScreens screens;
    EXPECT_TRUE(screens.open(dir.path(), "A.lui"));
    EXPECT_TRUE(screens.open(dir.path(), "B.lui"));
    EXPECT_FALSE(screens.open(dir.path(), "Missing.lui"));
    EXPECT_FALSE(screens.open(dir.path(), "../Outside.lui"));
    EXPECT_EQ(screens.openCount(), 2u);
    ASSERT_NE(screens.widget("Play"), nullptr);
    EXPECT_EQ(screens.widget("Play")->type, UiWidget::Type::Label) << "the latest opened first";
    EXPECT_TRUE(screens.open(dir.path(), "A.lui")) << "brought to the front";
    EXPECT_EQ(screens.widget("Play")->type, UiWidget::Type::Button);
    screens.close("A.lui");
    EXPECT_FALSE(screens.isOpen("A.lui"));
    EXPECT_TRUE(screens.isOpen("B.lui"));
    screens.close("");
    EXPECT_EQ(screens.openCount(), 0u);
}
