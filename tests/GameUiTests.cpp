#include "runtime/GameUi.hpp"

#include <gtest/gtest.h>

using namespace lumora;

namespace {

struct Ui {
    InputState input;
    HudCanvas hud;
    GameUi ui;
    void step() {
        hud.clear();
        ui.beginStep(&input, &hud);
    }
    void move(float x, float y) { input.setMousePosition({x, y}); }
    void down() { input.setMouseButton(MouseButton::Left, true); }
    void up() { input.setMouseButton(MouseButton::Left, false); }
};

constexpr glm::vec4 kRect{100.0f, 100.0f, 200.0f, 50.0f};

} // namespace

TEST(GameUi, ButtonClicksOnReleaseInsideOnce) {
    Ui t;
    t.step();
    t.move(150, 120);
    EXPECT_FALSE(t.ui.button("Play", kRect)) << "hovering is not clicking";
    t.down();
    t.step();
    EXPECT_FALSE(t.ui.button("Play", kRect)) << "held";
    t.step();
    EXPECT_FALSE(t.ui.button("Play", kRect));
    t.up();
    t.step();
    EXPECT_TRUE(t.ui.button("Play", kRect)) << "released over it";
    t.step();
    EXPECT_FALSE(t.ui.button("Play", kRect)) << "once";
}

TEST(GameUi, AClickShorterThanAStepStillCounts) {
    Ui t;
    t.step();
    t.move(150, 120);
    t.down();
    t.up();  // both between two steps
    t.step();
    EXPECT_TRUE(t.ui.button("Play", kRect));
    t.step();
    EXPECT_FALSE(t.ui.button("Play", kRect));
}

TEST(GameUi, DraggingOnOrOffAButtonIsNoClick) {
    Ui t;
    t.step();
    t.move(10, 10);  // press outside, release inside
    t.down();
    t.step();
    EXPECT_FALSE(t.ui.button("Quit", kRect));
    t.move(150, 120);
    t.up();
    t.step();
    EXPECT_FALSE(t.ui.button("Quit", kRect));
    t.down();  // press inside, release outside
    t.step();
    EXPECT_FALSE(t.ui.button("Quit", kRect));
    t.move(10, 10);
    t.up();
    t.step();
    EXPECT_FALSE(t.ui.button("Quit", kRect));
}

TEST(GameUi, PressesBeforeTheFirstStepAreIgnored) {
    Ui t;
    t.move(150, 120);
    t.down();
    t.up();
    t.step();
    EXPECT_FALSE(t.ui.button("Play", kRect)) << "a click from before play started";
}

TEST(GameUi, SliderFollowsTheMouseWhileHeld) {
    Ui t;
    float volume = 0.5f;
    t.step();
    t.move(150, 120);  // a quarter along
    t.down();
    t.step();
    EXPECT_TRUE(t.ui.slider("Volume", volume, 0.0f, 1.0f, kRect));
    EXPECT_NEAR(volume, 0.25f, 1e-5f);
    t.move(1000, 500);  // dragged far right: clamped
    t.step();
    t.ui.slider("Volume", volume, 0.0f, 1.0f, kRect);
    EXPECT_FLOAT_EQ(volume, 1.0f);
    t.up();
    t.step();
    t.ui.slider("Volume", volume, 0.0f, 1.0f, kRect);
    t.move(100, 120);
    t.step();
    EXPECT_FALSE(t.ui.slider("Volume", volume, 0.0f, 1.0f, kRect)) << "released: hovering changes nothing";
    EXPECT_FLOAT_EQ(volume, 1.0f);
}

TEST(GameUi, CheckboxReportsClicks) {
    Ui t;
    t.step();
    t.move(110, 120);
    t.down();
    t.up();
    t.step();
    EXPECT_TRUE(t.ui.checkbox("Fullscreen", false, kRect));
    t.step();
    EXPECT_FALSE(t.ui.checkbox("Fullscreen", true, kRect));
}

TEST(GameUi, KeysAndMouseEdgesOncePerPress) {
    Ui t;
    t.step();
    t.input.setKey(Key::Escape, true);
    t.input.setKey(Key::Escape, true);  // auto-repeat
    t.step();
    EXPECT_TRUE(t.ui.keyPressed(Key::Escape));
    t.step();
    EXPECT_FALSE(t.ui.keyPressed(Key::Escape)) << "still held, not pressed again";
    t.input.setKey(Key::Escape, false);
    t.input.setKey(Key::Escape, true);
    t.step();
    EXPECT_TRUE(t.ui.keyPressed(Key::Escape));
    t.down();
    t.step();
    EXPECT_TRUE(t.ui.pressed(MouseButton::Left));
    EXPECT_FALSE(t.ui.released(MouseButton::Left));
}

TEST(GameUi, MouseIsInHudPixelsAndWidgetsDrawInOrder) {
    Ui t;
    t.hud.pixelRatio = 2.0f;
    t.move(75, 60);  // window coordinates: HUD pixel (150, 120)
    t.step();
    EXPECT_EQ(t.ui.mouse(), glm::vec2(150.0f, 120.0f));
    t.ui.panel({0, 0, 400, 300});
    t.ui.button("Play", kRect);
    ASSERT_GE(t.hud.rects.size(), 2u);
    ASSERT_EQ(t.hud.texts.size(), 1u);
    EXPECT_EQ(t.hud.texts[0].utf8, "Play");
    EXPECT_EQ(t.hud.texts[0].align, HudCanvas::Align::Center);
    EXPECT_LT(t.hud.rects[0].order, t.hud.rects[1].order) << "panel under the button";
    EXPECT_LT(t.hud.rects[1].order, t.hud.texts[0].order) << "label over the button";
}
