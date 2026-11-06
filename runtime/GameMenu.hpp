#pragma once

#include "core/Error.hpp"

#include <filesystem>
#include <string>

namespace lumora {

class GameUi;
struct HudCanvas;
class Localization;
struct InputState;

/// The player's own settings for a game, kept next to its saves (settings.json).
struct GameSettings {
    float masterVolume = 1.0f;
    int quality = 2;  // 0 low, 1 medium, 2 high
    bool fullscreen = false;
    bool vsync = true;
    std::string language;  // empty = the system's

    [[nodiscard]] static GameSettings load(const std::filesystem::path& file);
    Result<> save(const std::filesystem::path& file) const;
    bool operator==(const GameSettings&) const = default;
};

/// The built-in pause menu of a game (Resume, Settings, Quit) and its settings page (volume,
/// graphics quality, fullscreen, vsync, language). Drawn with the game UI into the HUD; texts
/// come from the game's localization (keys "menu.resume", "menu.settings", ...) with English
/// defaults. Keyboard, mouse and gamepad.
class GameMenu {
public:
    enum class Action { None, Resume, Quit };

    void open() noexcept;
    /// Opens straight on the settings page (a game's own "Options" button).
    void openSettings() noexcept;
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] bool onSettingsPage() const noexcept { return settingsPage_; }

    /// Draws the menu for this frame (call between ui.beginStep and rendering). Changes go into
    /// `settings`; `changed` is set when one did.
    Action draw(GameUi& ui, HudCanvas& hud, GameSettings& settings, const Localization& texts, bool& changed);
    /// Escape / Back: the settings page goes back to the menu, the menu resumes the game.
    Action back();

private:
    bool open_ = false;
    bool settingsPage_ = false;
};

} // namespace lumora
