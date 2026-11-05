#pragma once

#include "core/Error.hpp"

#include <QString>

#include <cstdint>

namespace lumora::launcher {

struct ExtractLimits {
    std::uint64_t maxTotalBytes = 8ull << 30;  // guards against zip bombs
    std::uint32_t maxEntries = 200000;
};

struct ExtractStats {
    std::uint32_t files = 0;
    std::uint32_t directories = 0;
    std::uint32_t symlinks = 0;
    std::uint64_t bytes = 0;
};

/// Extracts a ZIP archive (stored or deflated) into `destination`, which is created.
///
/// Safe against hostile archives: every entry must stay inside the destination (no absolute
/// paths, drive letters, backslashes or ".." segments), symbolic links may only point to a
/// relative target inside it, and sizes are bounded. Unix permission bits (the executable bit
/// of binaries) are restored where the archive records them. On failure the destination may
/// hold a partial extraction; callers extract into a staging folder.
Result<ExtractStats> extractZip(const QString& archivePath, const QString& destination, const ExtractLimits& limits = {});

/// True if `name` is an acceptable archive entry path (see extractZip).
[[nodiscard]] bool isSafeEntryName(const QString& name);

} // namespace lumora::launcher
