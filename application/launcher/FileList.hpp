#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QString>

#include <vector>

namespace lumora::launcher {

/// One entry of a package's file list (<package>.files.json, published next to the ZIP).
struct FileEntry {
    QString path;        // relative, validated like ZIP entries
    quint64 size = 0;
    QByteArray sha256;   // lower-case hex; empty for links
    int mode = 0644;
    QString link;        // non-empty: symbolic link to this relative target
};

/// Parses and validates a file list ({"format": "lumora.files", "version": 1, "files": [...]}).
[[nodiscard]] Result<std::vector<FileEntry>> parseFileList(const QByteArray& json);
[[nodiscard]] QByteArray fileListJson(const std::vector<FileEntry>& entries);

/// Lists (and hashes) every file under `folder`, skipping the installer's own records - what an
/// installed version offers to later delta updates.
[[nodiscard]] Result<std::vector<FileEntry>> scanFolder(const QString& folder);

/// Name of the file list an installed editor keeps in its folder.
inline constexpr const char* kInstalledFileList = "lumora-files.json";

} // namespace lumora::launcher
