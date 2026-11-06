#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lumora {

/// The game's texts in several languages: Content/Localization/<code>.json, each a flat object
/// {"key": "text"} (the file name is the language code: en, pl, de, ...). Scripts show
/// tr("menu.play"); missing keys fall back to English, then to the key itself.
class Localization {
public:
    /// Reads every language file of the project. Returns how many were read.
    std::size_t load(const std::filesystem::path& projectRoot);
    /// Adds or replaces one language's texts (tests, tools).
    void setTexts(const std::string& language, std::map<std::string, std::string, std::less<>> texts);

    [[nodiscard]] std::vector<std::string> languages() const;
    /// False (and nothing changes) if the language has no file.
    bool setLanguage(std::string_view code);
    [[nodiscard]] const std::string& language() const noexcept { return current_; }
    /// Picks the first available of: `preferred`, the system language (`system`, e.g. "pl_PL"),
    /// English, the first language.
    void chooseLanguage(std::string_view preferred, std::string_view system);

    [[nodiscard]] std::string_view text(std::string_view key) const;
    /// Like text(), but `fallback` instead of the key when no language has it (built-in menus).
    [[nodiscard]] std::string_view text(std::string_view key, std::string_view fallback) const;

private:
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> texts_;
    std::string current_;
};

} // namespace lumora
