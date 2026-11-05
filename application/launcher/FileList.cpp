#include "launcher/FileList.hpp"

#include "launcher/ZipExtractor.hpp"

#include <nlohmann/json.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace lumora::launcher {

using nlohmann::json;

namespace {
constexpr std::uint64_t kMaxFileSize = 4ull << 30;
}

Result<std::vector<FileEntry>> parseFileList(const QByteArray& bytes) {
    const json root = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.value("format", std::string{}) != "lumora.files" ||
        root.value("version", 0) != 1) {
        return makeError("not a Lumora file list");
    }
    const auto files = root.find("files");
    if (files == root.end() || !files->is_array() || files->size() > 200000) return makeError("file list has no files");
    static const QRegularExpression hexSha(QStringLiteral("^[0-9a-f]{64}$"));
    std::vector<FileEntry> out;
    for (const json& f : *files) {
        if (!f.is_object()) return makeError("file list entry is not an object");
        FileEntry e;
        e.path = QString::fromStdString(f.value("path", std::string{}));
        if (!isSafeEntryName(e.path)) return makeError("unsafe path in file list: '{}'", e.path.toStdString());
        if (f.contains("link")) {
            e.link = QString::fromStdString(f.value("link", std::string{}));
            if (!isSafeEntryName(e.link)) return makeError("link '{}' points outside the package", e.path.toStdString());
        } else {
            e.size = f.value("size", std::uint64_t{0});
            e.sha256 = QByteArray::fromStdString(f.value("sha256", std::string{}));
            e.mode = f.value("mode", 0644) & 0777;
            if (e.size > kMaxFileSize || !hexSha.match(QString::fromLatin1(e.sha256)).hasMatch()) {
                return makeError("file list entry '{}' needs a size and sha256", e.path.toStdString());
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

QByteArray fileListJson(const std::vector<FileEntry>& entries) {
    json files = json::array();
    for (const FileEntry& e : entries) {
        if (!e.link.isEmpty()) {
            files.push_back({{"path", e.path.toStdString()}, {"link", e.link.toStdString()}});
        } else {
            files.push_back({{"path", e.path.toStdString()},
                             {"size", e.size},
                             {"sha256", e.sha256.toStdString()},
                             {"mode", e.mode}});
        }
    }
    return QByteArray::fromStdString(json{{"format", "lumora.files"}, {"version", 1}, {"files", files}}.dump(1));
}

Result<std::vector<FileEntry>> scanFolder(const QString& folder) {
    std::vector<FileEntry> out;
    const QDir root(folder);
    QDirIterator it(folder, QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info(path);
        FileEntry e;
        e.path = root.relativeFilePath(path);
        if (e.path.startsWith(QLatin1String("lumora-install.json")) || e.path == QLatin1String(kInstalledFileList)) continue;
        if (info.isSymLink()) {
            e.link = QDir(info.absolutePath()).relativeFilePath(info.symLinkTarget());
            if (!isSafeEntryName(e.link)) continue;  // not something a package could contain
            out.push_back(std::move(e));
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return makeError("cannot read {}", path.toStdString());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)) return makeError("cannot hash {}", path.toStdString());
        e.size = static_cast<quint64>(info.size());
        e.sha256 = hash.result().toHex();
        e.mode = info.isExecutable() ? 0755 : 0644;
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace lumora::launcher
