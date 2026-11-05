#include "project/ProjectRegistry.hpp"

#include "core/FileSystem.hpp"
#include "platform/Paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

namespace lumora {

namespace {
std::filesystem::path normalized(const std::filesystem::path& p) {
    return std::filesystem::absolute(p).lexically_normal();
}
} // namespace

ProjectRegistry::ProjectRegistry(std::filesystem::path storageFile) : storage_(std::move(storageFile)) {}

std::filesystem::path ProjectRegistry::defaultStorageFile() { return paths::userConfigDir() / "recent_projects.json"; }

Result<> ProjectRegistry::load() {
    entries_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(storage_, ec)) {
        return {};
    }
    auto text = fs::readTextFile(storage_);
    if (!text) return std::unexpected(text.error());
    const auto j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("projects") || !j["projects"].is_array()) {
        return makeError("recent project list '{}' is corrupt", storage_.string());
    }
    for (const auto& p : j["projects"]) {
        if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) continue;
        entries_.push_back({std::filesystem::path(p["path"].get<std::string>()), p.value("name", std::string{}),
                            p.value("lastOpened", std::int64_t{0})});
    }
    sort();
    return {};
}

Result<> ProjectRegistry::save() const {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& e : entries_) {
        list.push_back({{"path", e.descriptor.string()}, {"name", e.name}, {"lastOpened", e.lastOpenedUnix}});
    }
    return fs::writeTextFileAtomic(storage_, nlohmann::json{{"version", 1}, {"projects", list}}.dump(2) + "\n");
}

void ProjectRegistry::touch(const std::filesystem::path& descriptor, const std::string& name) {
    const auto path = normalized(descriptor);
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto it = std::ranges::find(entries_, path, &RecentProject::descriptor);
    if (it == entries_.end()) {
        entries_.push_back({path, name, now});
    } else {
        it->name = name;
        // Strictly increase so rapid successive touches keep a deterministic order.
        it->lastOpenedUnix = now;
    }
    for (auto& e : entries_) {
        if (e.descriptor != path && e.lastOpenedUnix >= now) {
            e.lastOpenedUnix = now - 1;
        }
    }
    sort();
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
}

bool ProjectRegistry::remove(const std::filesystem::path& descriptor) {
    return std::erase_if(entries_, [p = normalized(descriptor)](const auto& e) { return e.descriptor == p; }) > 0;
}

void ProjectRegistry::sort() {
    std::ranges::stable_sort(entries_, std::greater<>{}, &RecentProject::lastOpenedUnix);
}

} // namespace lumora
