#include "runtime/GameMenu.hpp"

#include "runtime/GameUi.hpp"
#include "runtime/Localization.hpp"
#include "scene/Hud.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace lumora {

GameSettings GameSettings::load(const std::filesystem::path& file) {
    GameSettings s;
    std::ifstream in(file);
    if (!in) return s;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return s;
    s.masterVolume = std::clamp(j.value("masterVolume", s.masterVolume), 0.0f, 1.0f);
    s.quality = std::clamp(j.value("quality", s.quality), 0, 2);
    s.fullscreen = j.value("fullscreen", s.fullscreen);
    s.vsync = j.value("vsync", s.vsync);
    s.language = j.value("language", s.language);
    return s;
}

Result<> GameSettings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const nlohmann::json j = {{"masterVolume", masterVolume}, {"quality", quality}, {"fullscreen", fullscreen},
                              {"vsync", vsync}, {"language", language}};
    std::ofstream out(file, std::ios::trunc);
    if (!(out << j.dump(2))) return makeError("could not write {}", file.string());
    return {};
}

namespace {

/// A language's own name for the menu (a language file can set its own: "language.name").
std::string_view nativeName(std::string_view code) {
    static const std::pair<std::string_view, std::string_view> kNames[] = {
        {"en", "English"},  {"pl", "Polski"},    {"de", "Deutsch"},   {"fr", "Français"},
        {"es", "Español"},  {"it", "Italiano"},  {"pt", "Português"}, {"cs", "Čeština"},
        {"uk", "Українська"}, {"ru", "Русский"}, {"nl", "Nederlands"}, {"sv", "Svenska"},
    };
    for (const auto& [c, name] : kNames) {
        if (c == code) return name;
    }
    return code;
}

} // namespace

void GameMenu::open() noexcept {
    open_ = true;
    settingsPage_ = false;
}

void GameMenu::openSettings() noexcept {
    open_ = true;
    settingsPage_ = true;
}

GameMenu::Action GameMenu::back() {
    if (settingsPage_) {
        settingsPage_ = false;
        return Action::None;
    }
    open_ = false;
    return Action::Resume;
}

GameMenu::Action GameMenu::draw(GameUi& ui, HudCanvas& hud, GameSettings& settings, const Localization& texts, bool& changed) {
    if (!open_) return Action::None;
    auto t = [&](const char* key, const char* english) { return std::string(texts.text(key, english)); };
    const glm::vec2 screen = hud.screenSize;
    const float s = std::max(screen.y / 720.0f, 0.75f);
    ui.style().fontSize = 22.0f * s;
    const float width = 420.0f * s, row = 46.0f * s, gap = 12.0f * s;
    const int rows = settingsPage_ ? (texts.languages().size() > 1 ? 7 : 6) : 4;
    const float height = static_cast<float>(rows) * (row + gap) + gap;
    const glm::vec2 corner = (screen - glm::vec2(width, height)) * 0.5f;
    hud.addRect({0.0f, 0.0f}, screen, {0.0f, 0.0f, 0.0f, 0.45f});  // dim the game
    ui.panel({corner.x, corner.y, width, height});
    const float x = corner.x + gap, w = width - 2.0f * gap;
    float y = corner.y + gap;
    auto next = [&] {
        const glm::vec4 r{x, y, w, row};
        y += row + gap;
        return r;
    };
    const glm::vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
    if (!settingsPage_) {
        ui.label(t("menu.paused", "Paused"), next(), HudCanvas::Align::Center, white);
        if (ui.button(t("menu.resume", "Resume"), next())) {
            open_ = false;
            return Action::Resume;
        }
        if (ui.button(t("menu.settings", "Settings"), next())) settingsPage_ = true;
        if (ui.button(t("menu.quit", "Quit"), next())) return Action::Quit;
        return Action::None;
    }
    ui.label(t("menu.settings", "Settings"), next(), HudCanvas::Align::Center, white);
    float volume = settings.masterVolume;
    if (ui.slider(t("menu.volume", "Volume"), volume, 0.0f, 1.0f, next())) {
        settings.masterVolume = volume;
        changed = true;
    }
    static const char* const kQualityKeys[3] = {"menu.quality.low", "menu.quality.medium", "menu.quality.high"};
    static const char* const kQualityNames[3] = {"Low", "Medium", "High"};
    const int q = std::clamp(settings.quality, 0, 2);
    if (ui.button(t("menu.quality", "Graphics") + ": " + t(kQualityKeys[q], kQualityNames[q]), next())) {
        settings.quality = (q + 1) % 3;
        changed = true;
    }
    if (ui.checkbox(t("menu.fullscreen", "Fullscreen"), settings.fullscreen, next())) {
        settings.fullscreen = !settings.fullscreen;
        changed = true;
    }
    if (ui.checkbox(t("menu.vsync", "VSync"), settings.vsync, next())) {
        settings.vsync = !settings.vsync;
        changed = true;
    }
    const std::vector<std::string> languages = texts.languages();
    if (languages.size() > 1) {
        const std::string& current = texts.language();
        const std::string name(texts.text("language.name", nativeName(current)));
        if (ui.button(t("menu.language", "Language") + ": " + name, next())) {
            auto it = std::ranges::find(languages, current);
            settings.language = (it == languages.end() || it + 1 == languages.end()) ? languages.front() : *(it + 1);
            changed = true;
        }
    }
    if (ui.button(t("menu.back", "Back"), next())) settingsPage_ = false;
    return Action::None;
}

} // namespace lumora
