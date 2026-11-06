#include "TestUtils.hpp"

#include "runtime/GameMenu.hpp"
#include "runtime/GameUi.hpp"
#include "runtime/Localization.hpp"
#include "scene/Hud.hpp"

#include <gtest/gtest.h>

#include <fstream>

using namespace lumora;

TEST(Localization, ReadsLanguageFilesAndFallsBack) {
    test::TempDir dir;
    const auto folder = dir.path() / "Content" / "Localization";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "en.json") << R"({"menu.play": "Play", "menu.quit": "Quit", "only.en": "English only"})";
    std::ofstream(folder / "pl.json") << R"({"menu.play": "Graj", "menu.quit": "Wyjdź", "language.name": "Polski"})";
    std::ofstream(folder / "broken.json") << "[1, 2";
    Localization texts;
    EXPECT_EQ(texts.load(dir.path()), 2u) << "a broken file is skipped";
    EXPECT_EQ(texts.languages(), (std::vector<std::string>{"en", "pl"}));
    EXPECT_EQ(texts.language(), "en") << "English by default";
    EXPECT_EQ(texts.text("menu.play"), "Play");
    texts.chooseLanguage({}, "pl_PL");
    EXPECT_EQ(texts.language(), "pl") << "the system language";
    EXPECT_EQ(texts.text("menu.quit"), "Wyjdź");
    EXPECT_EQ(texts.text("only.en"), "English only") << "missing: English";
    EXPECT_EQ(texts.text("nowhere"), "nowhere") << "missing everywhere: the key";
    EXPECT_EQ(texts.text("nowhere", "Default"), "Default");
    EXPECT_FALSE(texts.setLanguage("de"));
    EXPECT_EQ(texts.language(), "pl");
    texts.chooseLanguage("en", "pl_PL");
    EXPECT_EQ(texts.language(), "en") << "the player's choice wins";
}

TEST(GameMenu, PagesSettingsAndSaving) {
    test::TempDir dir;
    GameSettings settings;
    settings.masterVolume = 0.4f;
    settings.quality = 1;
    settings.language = "pl";
    ASSERT_TRUE(settings.save(dir.path() / "s" / "settings.json").has_value());
    EXPECT_EQ(GameSettings::load(dir.path() / "s" / "settings.json"), settings);
    EXPECT_EQ(GameSettings::load(dir.path() / "missing.json"), GameSettings{}) << "defaults without a file";

    Localization texts;
    texts.setTexts("pl", {{"menu.resume", "Wznów"}});
    GameMenu menu;
    GameUi ui;
    HudCanvas hud;
    bool changed = false;
    EXPECT_EQ(menu.draw(ui, hud, settings, texts, changed), GameMenu::Action::None) << "closed: nothing";
    EXPECT_TRUE(hud.empty());
    menu.open();
    ui.beginStep(nullptr, &hud);
    menu.draw(ui, hud, settings, texts, changed);
    bool translated = false, english = false;
    for (const auto& t : hud.texts) {
        translated = translated || t.utf8 == "Wznów";
        english = english || t.utf8 == "Settings";
    }
    EXPECT_TRUE(translated) << "texts from the game's language";
    EXPECT_TRUE(english) << "built-in English for the rest";
    EXPECT_FALSE(changed);
    EXPECT_EQ(menu.back(), GameMenu::Action::Resume) << "Escape on the main page resumes";
    EXPECT_FALSE(menu.isOpen());
}
