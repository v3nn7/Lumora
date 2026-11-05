#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lumora::fs {

[[nodiscard]] Result<std::string> readTextFile(const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path);

/// Writes via a temporary sibling file followed by rename, so readers never observe partial content.
[[nodiscard]] Result<> writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data);
[[nodiscard]] Result<> writeTextFileAtomic(const std::filesystem::path& path, std::string_view text);

} // namespace lumora::fs
