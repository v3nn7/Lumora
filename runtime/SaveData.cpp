#include "runtime/SaveData.hpp"

#include "core/FileSystem.hpp"
#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <system_error>

namespace lumora {

SaveData::SaveData(std::filesystem::path file) : file_(std::move(file)) {
    std::error_code ec;
    if (file_.empty() || !std::filesystem::is_regular_file(file_, ec)) return;
    auto text = fs::readTextFile(file_);
    const nlohmann::json json = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json();
    if (!text || json.is_discarded() || !json.is_object()) {
        log::warn("Save", "{} is not a save file - starting with no saved values", file_.string());
        return;
    }
    for (const auto& [key, value] : json.items()) {
        if (value.is_number()) {
            values_[key] = value.get<double>();
        } else if (value.is_string()) {
            values_[key] = value.get<std::string>();
        }
    }
}

SaveData::~SaveData() {
    if (auto written = flush(); !written) log::error("Save", "{}", written.error().message);
}

void SaveData::setNumber(std::string_view key, double value) {
    auto it = values_.find(key);
    if (it != values_.end() && std::holds_alternative<double>(it->second) && std::get<double>(it->second) == value) return;
    values_[std::string(key)] = value;
    dirty_ = true;
}

void SaveData::setText(std::string_view key, std::string_view value) {
    auto it = values_.find(key);
    if (it != values_.end() && std::holds_alternative<std::string>(it->second) && std::get<std::string>(it->second) == value) {
        return;
    }
    values_[std::string(key)] = std::string(value);
    dirty_ = true;
}

std::optional<double> SaveData::number(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end() || !std::holds_alternative<double>(it->second)) return std::nullopt;
    return std::get<double>(it->second);
}

std::optional<std::string> SaveData::text(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end() || !std::holds_alternative<std::string>(it->second)) return std::nullopt;
    return std::get<std::string>(it->second);
}

bool SaveData::has(std::string_view key) const { return values_.find(key) != values_.end(); }

void SaveData::erase(std::string_view key) {
    if (const auto it = values_.find(key); it != values_.end()) {
        values_.erase(it);
        dirty_ = true;
    }
}

void SaveData::clear() {
    if (values_.empty()) return;
    values_.clear();
    dirty_ = true;
}

Result<> SaveData::flush() {
    if (!dirty_ || file_.empty()) return {};
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [key, value] : values_) {
        std::visit([&](const auto& v) { json[key] = v; }, value);
    }
    std::error_code ec;
    if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
    if (auto written = fs::writeTextFileAtomic(file_, json.dump(2) + "\n"); !written) return written;
    dirty_ = false;
    lastWrite_ = std::chrono::steady_clock::now();
    return {};
}

void SaveData::flushIfDue() {
    if (!dirty_ || std::chrono::steady_clock::now() - lastWrite_ < std::chrono::seconds(1)) return;
    if (auto written = flush(); !written) log::error("Save", "{}", written.error().message);
}

} // namespace lumora
