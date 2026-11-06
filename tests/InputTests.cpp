#include "TestUtils.hpp"

#include "runtime/GameUi.hpp"
#include "runtime/InputActions.hpp"
#include "runtime/SaveData.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>

using namespace lumora;

TEST(InputActions, BindingsParseAndPrint) {
    for (const char* name : {"Space", "W", "Num7", "Enter", "Mouse.Left", "Gamepad.A", "Gamepad.DPadUp",
                             "Gamepad.RightTrigger", "Gamepad.LeftX-", "Gamepad.RightY+"}) {
        auto b = InputBinding::parse(name);
        ASSERT_TRUE(b.has_value()) << name;
        EXPECT_EQ(b->name(), name);
    }
    EXPECT_FALSE(InputBinding::parse("Jump").has_value());
    EXPECT_FALSE(InputBinding::parse("Gamepad.Q").has_value());
    EXPECT_FALSE(InputBinding::parse("Mouse.Fourth").has_value());
}

TEST(InputActions, KeyboardAndGamepadDriveTheSameActions) {
    InputActions actions = InputActions::defaults();
    InputState input;
    actions.beginStep(&input);
    EXPECT_FALSE(actions.down("Jump"));
    input.setKey(Key::Space, true);
    actions.beginStep(&input);
    EXPECT_TRUE(actions.down("Jump"));
    EXPECT_TRUE(actions.pressed("Jump"));
    actions.beginStep(&input);
    EXPECT_FALSE(actions.pressed("Jump")) << "held, not pressed again";
    input.setKey(Key::Space, false);
    input.setGamepadConnected(true);
    input.setGamepadButton(GamepadButton::A, true);
    actions.beginStep(&input);
    EXPECT_TRUE(actions.pressed("Jump")) << "the gamepad's A";
    input.setGamepadAxis(GamepadAxis::RightTrigger, 0.9f);
    actions.beginStep(&input);
    EXPECT_TRUE(actions.down("Fire")) << "a trigger pulled past half";
    EXPECT_FALSE(actions.down("Nonexistent"));
}

TEST(InputActions, AxesCombineKeysAndSticks) {
    InputActions actions = InputActions::defaults();
    InputState input;
    actions.beginStep(&input);
    EXPECT_EQ(actions.axis("MoveY"), 0.0f);
    input.setKey(Key::W, true);
    EXPECT_EQ(actions.axis("MoveY"), 1.0f) << "W is forward";
    input.setKey(Key::S, true);
    EXPECT_EQ(actions.axis("MoveY"), 0.0f) << "both cancel";
    input.setKey(Key::W, false);
    input.setKey(Key::S, false);
    input.setGamepadAxis(GamepadAxis::LeftY, -0.6f);  // stick pushed up
    EXPECT_NEAR(actions.axis("MoveY"), 0.6f, 1e-6f) << "inverted: up is forward";
    input.setGamepadAxis(GamepadAxis::LeftX, 0.3f);
    EXPECT_NEAR(actions.axis("MoveX"), 0.3f, 1e-6f);
    input.setKey(Key::A, true);
    EXPECT_EQ(actions.axis("MoveX"), -1.0f) << "whichever is pushed further";
}

TEST(InputActions, ProjectSettingsOverrideAndAdd) {
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Config");
    std::ofstream(project.path() / "Config" / "Input.json") << R"({
        "actions": { "Jump": ["J"], "Dash": ["LeftShift", "Gamepad.RightBumper"] },
        "axes": { "Throttle": { "positive": ["Up"], "gamepad": "RightTrigger" } } })";
    auto actions = InputActions::forProject(project.path());
    ASSERT_TRUE(actions.has_value()) << actions.error().message;
    InputState input;
    input.setKey(Key::Space, true);
    actions->beginStep(&input);
    EXPECT_FALSE(actions->down("Jump")) << "Jump moved to J";
    input.setKey(Key::J, true);
    EXPECT_TRUE(actions->down("Jump"));
    input.setGamepadButton(GamepadButton::RightBumper, true);
    EXPECT_TRUE(actions->down("Dash"));
    EXPECT_TRUE(actions->actions().contains("Fire")) << "the other defaults stay";
    input.setGamepadAxis(GamepadAxis::RightTrigger, 0.4f);
    EXPECT_NEAR(actions->axis("Throttle"), 0.4f, 1e-6f);
    // The settings round-trip through JSON.
    InputActions copy = InputActions::defaults();
    ASSERT_TRUE(copy.apply(actions->toJson()).has_value());
    EXPECT_EQ(copy.actions(), actions->actions());

    std::ofstream(project.path() / "Config" / "Input.json") << R"({ "actions": { "Jump": ["Spacebar"] } })";
    auto broken = InputActions::forProject(project.path());
    ASSERT_FALSE(broken.has_value());
    EXPECT_NE(broken.error().message.find("Spacebar"), std::string::npos) << "names the bad input";
}

TEST(SaveData, KeepsNumbersAndTextsBetweenRuns) {
    test::TempDir dir;
    const auto file = dir.path() / "Saves" / "save.json";
    {
        SaveData save(file);
        EXPECT_EQ(save.size(), 0u);
        save.setNumber("highScore", 1250);
        save.setText("name", "Zażółć");
        save.setNumber("level", 3);
        save.erase("level");
        EXPECT_FALSE(save.has("level"));
    }  // written when it goes away
    SaveData again(file);
    EXPECT_EQ(again.number("highScore"), 1250.0);
    EXPECT_EQ(again.text("name"), std::string("Zażółć"));
    EXPECT_FALSE(again.number("name").has_value()) << "a text is not a number";
    EXPECT_FALSE(again.has("level"));
    again.clear();
    ASSERT_TRUE(again.flush().has_value());
    EXPECT_EQ(SaveData(file).size(), 0u);

    std::ofstream(file) << "{ broken";
    EXPECT_EQ(SaveData(file).size(), 0u) << "a damaged file starts empty instead of failing";
}

TEST(GameUi, KeyboardAndGamepadMoveTheFocus) {
    InputState input;
    HudCanvas hud;
    GameUi ui;
    float volume = 0.5f;
    auto frame = [&](GameUi::Navigation nav, bool& play, bool& quit) {
        hud.clear();
        ui.beginStep(&input, &hud, nav);
        play = ui.button("Play", {0, 0, 200, 50});
        ui.slider("Volume", volume, 0.0f, 1.0f, {0, 60, 200, 40});
        quit = ui.button("Quit", {0, 110, 200, 50});
    };
    bool play = false, quit = false;
    frame({}, play, quit);
    EXPECT_EQ(ui.focus(), -1) << "the mouse leads at first";
    frame({.down = true}, play, quit);
    EXPECT_EQ(ui.focus(), 0);
    frame({.down = true}, play, quit);
    frame({.right = true}, play, quit);
    EXPECT_NEAR(volume, 0.55f, 1e-5f) << "Right nudges the focused slider";
    frame({.down = true}, play, quit);
    frame({.submit = true}, play, quit);
    EXPECT_TRUE(quit) << "Submit clicks the focused button";
    EXPECT_FALSE(play);
    frame({.down = true}, play, quit);
    EXPECT_EQ(ui.focus(), 0) << "wraps around";
    frame({.up = true}, play, quit);
    EXPECT_EQ(ui.focus(), 2);
    input.setMousePosition({300.0f, 300.0f});
    frame({}, play, quit);
    EXPECT_EQ(ui.focus(), -1) << "moving the mouse hands control back";
}
