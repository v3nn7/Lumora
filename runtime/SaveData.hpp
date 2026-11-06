#pragma once

#include "core/Error.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace lumora {

/// The game's saved values (progress, high scores, settings): numbers and texts by key, kept
/// in a JSON file that survives between runs. Changes are written at most once a second while
/// playing (flushIfDue) and when the game stops (flush, the destructor).
class SaveData {
public:
    using Value = std::variant<double, std::string>;

    SaveData() = default;
    /// Reads `file` if it exists; an unreadable file is reported and starts empty.
    explicit SaveData(std::filesystem::path file);
    ~SaveData();
    SaveData(const SaveData&) = delete;
    SaveData& operator=(const SaveData&) = delete;

    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    void setNumber(std::string_view key, double value);
    void setText(std::string_view key, std::string_view value);
    [[nodiscard]] std::optional<double> number(std::string_view key) const;
    [[nodiscard]] std::optional<std::string> text(std::string_view key) const;
    [[nodiscard]] bool has(std::string_view key) const;
    void erase(std::string_view key);
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

    /// Writes the file now if something changed (atomically).
    Result<> flush();
    /// Writes if something changed and the last write is a second old.
    void flushIfDue();

private:
    std::filesystem::path file_;
    std::map<std::string, Value, std::less<>> values_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point lastWrite_{};
};

} // namespace lumora
