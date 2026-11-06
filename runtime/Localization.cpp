#include "runtime/Localization.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace lumora {

std::size_t Localization::load(const std::filesystem::path& projectRoot) {
    texts_.clear();
    const std::filesystem::path folder = projectRoot / "Content" / "Localization";
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream in(entry.path());
        const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (!json.is_object()) {
            log::warn("Localization", "{} is not a JSON object of texts", entry.path().filename().string());
            continue;
        }
        auto& texts = texts_[entry.path().stem().string()];
        for (const auto& [key, value] : json.items()) {
            if (value.is_string()) texts[key] = value.get<std::string>();
        }
    }
    if (!current_.empty() && !texts_.contains(current_)) current_.clear();
    if (current_.empty()) chooseLanguage({}, {});
    return texts_.size();
}

void Localization::setTexts(const std::string& language, std::map<std::string, std::string, std::less<>> texts) {
    texts_[language] = std::move(texts);
    if (current_.empty()) current_ = language;
}

std::vector<std::string> Localization::languages() const {
    std::vector<std::string> out;
    for (const auto& [code, texts] : texts_) out.push_back(code);
    return out;
}

bool Localization::setLanguage(std::string_view code) {
    const auto it = texts_.find(code);
    if (it == texts_.end()) return false;
    current_ = it->first;
    return true;
}

void Localization::chooseLanguage(std::string_view preferred, std::string_view system) {
    if (!preferred.empty() && setLanguage(preferred)) return;
    if (system.size() >= 2) {
        if (setLanguage(system)) return;
        if (setLanguage(system.substr(0, 2))) return;  // "pl_PL" -> "pl"
    }
    if (setLanguage("en")) return;
    current_ = texts_.empty() ? std::string() : texts_.begin()->first;
}

std::string_view Localization::text(std::string_view key) const { return text(key, key); }

std::string_view Localization::text(std::string_view key, std::string_view fallback) const {
    for (const std::string_view language : {std::string_view(current_), std::string_view("en")}) {
        const auto texts = texts_.find(language);
        if (texts == texts_.end()) continue;
        if (const auto it = texts->second.find(key); it != texts->second.end()) return it->second;
    }
    return fallback;
}

} // namespace lumora
