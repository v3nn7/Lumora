#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lumora {

struct RecentProject {
    std::filesystem::path descriptor;  // absolute path to <Name>.lumora
    std::string name;
    std::int64_t lastOpenedUnix = 0;
};

/// Persistent list of recently used projects (used by the Hub). Stored as JSON.
class ProjectRegistry {
public:
    explicit ProjectRegistry(std::filesystem::path storageFile);

    /// Default storage under the user config directory.
    [[nodiscard]] static std::filesystem::path defaultStorageFile();

    [[nodiscard]] Result<> load();
    [[nodiscard]] Result<> save() const;

    /// Inserts or refreshes an entry and marks it as most recently opened.
    void touch(const std::filesystem::path& descriptor, const std::string& name);
    bool remove(const std::filesystem::path& descriptor);
    /// Most recent first.
    [[nodiscard]] const std::vector<RecentProject>& entries() const noexcept { return entries_; }

    static constexpr std::size_t kMaxEntries = 64;

private:
    void sort();
    std::filesystem::path storage_;
    std::vector<RecentProject> entries_;
};

} // namespace lumora
