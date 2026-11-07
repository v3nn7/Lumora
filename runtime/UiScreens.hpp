#pragma once

#include "runtime/UiDocument.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace lumora {

class GameUi;
class Localization;
class InputState;
struct HudCanvas;

/// The UI documents (.lui) open in a running game: drawn every step with the game UI (focus,
/// pad navigation and clicks as for immediate widgets), in the order opened (later on top).
/// Widget values and texts live here, so scripts read and change them by name.
class UiScreens {
public:
    /// Opens a document (project-relative key) or brings an open one to the front; false if it
    /// cannot be read (logged).
    bool open(const std::filesystem::path& projectRoot, const std::string& key);
    /// Closes a document; an empty key closes all.
    void close(const std::string& key);
    [[nodiscard]] bool isOpen(const std::string& key) const;
    [[nodiscard]] std::size_t openCount() const noexcept { return open_.size(); }

    /// Draws the open documents (hud: its screen size) and records clicks and changes.
    void draw(GameUi& ui, HudCanvas& hud, const InputState* input, const Localization* texts);

    /// A button (or checkbox) with this name was clicked in the latest step.
    [[nodiscard]] bool clicked(std::string_view name) const { return clicked_.contains(std::string(name)); }
    /// A checkbox or slider with this name changed in the latest step.
    [[nodiscard]] bool changed(std::string_view name) const { return changed_.contains(std::string(name)); }
    /// The widget with this name in the open documents (the latest opened first), or null.
    [[nodiscard]] UiWidget* widget(std::string_view name);

private:
    struct Open {
        std::string key;
        UiDocument document;
        std::map<std::string, float> scroll;  // scrolled panels, by path
    };
    std::vector<Open> open_;
    std::set<std::string> clicked_;
    std::set<std::string> changed_;
};

} // namespace lumora
