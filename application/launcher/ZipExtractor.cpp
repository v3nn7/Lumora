#include "launcher/ZipExtractor.hpp"

#include "core/Log.hpp"

#include <miniz.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <vector>

#include <filesystem>
#include <memory>

namespace lumora::launcher {

namespace {

constexpr unsigned kUnixHost = 3;
constexpr std::uint32_t kTypeMask = 0170000;
constexpr std::uint32_t kSymlink = 0120000;
constexpr std::uint32_t kDirectory = 0040000;

size_t readFromFile(void* opaque, mz_uint64 offset, void* buffer, size_t n) {
    auto* file = static_cast<QFile*>(opaque);
    if (!file->seek(static_cast<qint64>(offset))) return 0;
    const qint64 got = file->read(static_cast<char*>(buffer), static_cast<qint64>(n));
    return got < 0 ? 0 : static_cast<size_t>(got);
}

struct WriteTarget {
    QFile* file = nullptr;
    std::uint64_t written = 0;
    std::uint64_t budget = 0;  // bytes this entry may still write
    bool overflow = false;
};

size_t writeToFile(void* opaque, mz_uint64, const void* buffer, size_t n) {
    auto* target = static_cast<WriteTarget*>(opaque);
    if (target->written + n > target->budget) {  // the header lied about the size
        target->overflow = true;
        return 0;
    }
    const qint64 wrote = target->file->write(static_cast<const char*>(buffer), static_cast<qint64>(n));
    if (wrote != static_cast<qint64>(n)) return 0;
    target->written += n;
    return n;
}

QFileDevice::Permissions permissionsFrom(std::uint32_t mode) {
    QFileDevice::Permissions p;
    if (mode & 0400) p |= QFileDevice::ReadOwner | QFileDevice::ReadUser;
    if (mode & 0200) p |= QFileDevice::WriteOwner | QFileDevice::WriteUser;
    if (mode & 0100) p |= QFileDevice::ExeOwner | QFileDevice::ExeUser;
    if (mode & 0040) p |= QFileDevice::ReadGroup;
    if (mode & 0020) p |= QFileDevice::WriteGroup;
    if (mode & 0010) p |= QFileDevice::ExeGroup;
    if (mode & 0004) p |= QFileDevice::ReadOther;
    if (mode & 0002) p |= QFileDevice::WriteOther;
    if (mode & 0001) p |= QFileDevice::ExeOther;
    return p;
}

struct ZipCloser {
    void operator()(mz_zip_archive* zip) const { mz_zip_reader_end(zip); }
};

} // namespace

bool isSafeEntryName(const QString& name) {
    if (name.isEmpty() || name.size() > 400 || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        name.contains(QLatin1Char(':')) || name.contains(QChar(0))) {
        return false;
    }
    QString path = name;
    if (path.endsWith(QLatin1Char('/'))) path.chop(1);  // directory entries
    const QStringList parts = path.split(QLatin1Char('/'));
    return std::ranges::none_of(parts, [](const QString& part) {
        return part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..");
    });
}

Result<ExtractStats> extractZip(const QString& archivePath, const QString& destination, const ExtractLimits& limits) {
    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadOnly)) {
        return makeError("cannot open {}: {}", archivePath.toStdString(), archive.errorString().toStdString());
    }
    auto zip = std::make_unique<mz_zip_archive>();
    mz_zip_zero_struct(zip.get());
    zip->m_pRead = readFromFile;
    zip->m_pIO_opaque = &archive;
    if (!mz_zip_reader_init(zip.get(), static_cast<mz_uint64>(archive.size()), 0)) {
        return makeError("{} is not a valid ZIP archive ({})", archivePath.toStdString(),
                         mz_zip_get_error_string(mz_zip_get_last_error(zip.get())));
    }
    std::unique_ptr<mz_zip_archive, ZipCloser> guard(zip.release());
    mz_zip_archive* z = guard.get();

    if (!QDir().mkpath(destination)) return makeError("cannot create {}", destination.toStdString());
    const QString root = QFileInfo(destination).canonicalFilePath();
    const QDir rootDir(root);
    const mz_uint count = mz_zip_reader_get_num_files(z);
    if (count > limits.maxEntries) return makeError("archive has too many entries ({})", count);

    // Validate everything (names, sizes) before writing anything.
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(z, i, &stat)) return makeError("corrupt archive entry {}", i);
        const QString name = QString::fromUtf8(stat.m_filename);
        if (!isSafeEntryName(name)) return makeError("unsafe path in archive: '{}'", name.toStdString());
        if (stat.m_is_encrypted || !stat.m_is_supported) {
            return makeError("unsupported (encrypted or unknown method) entry '{}'", name.toStdString());
        }
        const std::uint32_t mode = (stat.m_version_made_by >> 8) == kUnixHost ? (stat.m_external_attr >> 16) : 0;
        if ((mode & kTypeMask) == kSymlink) {
            // Only plain relative targets: no absolute paths and no ".." (which would resolve
            // through other links and could leave the folder).
            if (stat.m_uncomp_size == 0 || stat.m_uncomp_size > 1024) return makeError("invalid link '{}'", name.toStdString());
            QByteArray linkTarget(static_cast<qsizetype>(stat.m_uncomp_size), '\0');
            if (!mz_zip_reader_extract_to_mem(z, i, linkTarget.data(), static_cast<size_t>(linkTarget.size()), 0) ||
                !isSafeEntryName(QString::fromUtf8(linkTarget))) {
                return makeError("link '{}' points outside the package", name.toStdString());
            }
        }
        total += stat.m_uncomp_size;
        if (total > limits.maxTotalBytes) return makeError("archive expands beyond the allowed size");
    }

    auto insideRoot = [&](const QString& absolute) {
        const QString canonical = QFileInfo(absolute).canonicalFilePath();
        return !canonical.isEmpty() && (canonical == root || canonical.startsWith(root + QLatin1Char('/')));
    };

    ExtractStats stats;
    std::vector<std::pair<QString, QString>> links;  // Windows: (link, its target), copied at the end
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        mz_zip_reader_file_stat(z, i, &stat);
        const QString name = QString::fromUtf8(stat.m_filename);
        const bool unixMode = (stat.m_version_made_by >> 8) == kUnixHost;
        const std::uint32_t mode = unixMode ? (stat.m_external_attr >> 16) : 0;
        const QString target = rootDir.filePath(name);

        if (stat.m_is_directory || (mode & kTypeMask) == kDirectory) {
            if (!QDir().mkpath(target) || !insideRoot(target)) return makeError("cannot create folder '{}'", name.toStdString());
            ++stats.directories;
            continue;
        }
        const QString parent = QFileInfo(target).absolutePath();
        if (!QDir().mkpath(parent) || !insideRoot(parent)) {
            return makeError("cannot create folder for '{}'", name.toStdString());
        }

        if ((mode & kTypeMask) == kSymlink) {
            if (stat.m_uncomp_size == 0 || stat.m_uncomp_size > 1024) return makeError("invalid link '{}'", name.toStdString());
            QByteArray linkTarget(static_cast<qsizetype>(stat.m_uncomp_size), '\0');
            if (!mz_zip_reader_extract_to_mem(z, i, linkTarget.data(), static_cast<size_t>(linkTarget.size()), 0)) {
                return makeError("cannot read link '{}'", name.toStdString());
            }
            const QString to = QString::fromUtf8(linkTarget);  // validated above
#if defined(Q_OS_WIN)
            // No symbolic links without developer mode: the link becomes a copy of its target
            // (a Linux package's libfoo.so -> libfoo.so.6, for exporting Linux games).
            links.emplace_back(target, QFileInfo(target).dir().filePath(to));
#else
            std::error_code ec;
            std::filesystem::remove(target.toStdString(), ec);
            std::filesystem::create_symlink(to.toStdString(), target.toStdString(), ec);
            if (ec) return makeError("cannot create link '{}': {}", name.toStdString(), ec.message());
            ++stats.symlinks;
#endif
            continue;
        }

        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return makeError("cannot write '{}': {}", name.toStdString(), out.errorString().toStdString());
        }
        WriteTarget sink{&out, 0, stat.m_uncomp_size, false};
        if (!mz_zip_reader_extract_to_callback(z, i, writeToFile, &sink, 0) || sink.written != stat.m_uncomp_size) {
            return makeError("cannot extract '{}'{}", name.toStdString(), sink.overflow ? " (size mismatch)" : "");
        }
        out.close();
        if (mode != 0) out.setPermissions(permissionsFrom(mode & 0777));
        ++stats.files;
        stats.bytes += sink.written;
    }
    // Links to links resolve over a few passes.
    for (int pass = 0; pass < 8 && !links.empty(); ++pass) {
        std::erase_if(links, [&](const std::pair<QString, QString>& link) {
            if (!QFileInfo(link.second).isFile() || !insideRoot(link.second)) return false;
            QFile::remove(link.first);
            if (!QFile::copy(link.second, link.first)) return false;
            ++stats.symlinks;  // a link in the package, stored as a copy here
            return true;
        });
    }
    for (const auto& [link, to] : links) log::warn("Launcher", "skipping link {} (target {} missing)", link.toStdString(), to.toStdString());
    return stats;
}

} // namespace lumora::launcher
