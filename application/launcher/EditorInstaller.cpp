#include "launcher/EditorInstaller.hpp"

#include "core/Log.hpp"
#include "core/Version.hpp"
#include "launcher/FileList.hpp"
#include "launcher/Signature.hpp"
#include "launcher/ZipExtractor.hpp"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <filesystem>
#include <map>

namespace lumora::launcher {

using nlohmann::json;

namespace {

constexpr qint64 kMaxFeedBytes = 4 << 20;
constexpr const char* kInstallFile = "lumora-install.json";

std::optional<InstalledEditor> readInstall(const QString& directory) {
    QFile file(QDir(directory).filePath(QString::fromLatin1(kInstallFile)));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return std::nullopt;
    const QByteArray bytes = file.readAll();
    const json j = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    InstalledEditor e;
    e.version = QString::fromStdString(j.value("version", std::string{}));
    const QString relative = QString::fromStdString(j.value("executable", std::string{}));
    if (!isValidVersion(e.version) || relative.isEmpty() || relative.contains(QLatin1String(".."))) return std::nullopt;
    e.directory = QFileInfo(directory).absoluteFilePath();
    e.executable = QDir(e.directory).filePath(relative);
    e.source = QString::fromStdString(j.value("source", std::string{}));
    e.installedAt = QDateTime::fromString(QString::fromStdString(j.value("installedAt", std::string{})), Qt::ISODate);
    if (!QFileInfo(e.executable).isFile()) return std::nullopt;
    return e;
}

/// Packages may wrap everything in one top-level folder; the content is wherever bin/ is.
QString contentFolder(const QString& staging, const QString& executable = editorExecutable()) {
    if (QFileInfo::exists(QDir(staging).filePath(executable))) return staging;
    const QStringList entries = QDir(staging).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    if (entries.size() == 1) {
        const QString inner = QDir(staging).filePath(entries.first());
        if (QFileInfo::exists(QDir(inner).filePath(executable))) return inner;
    }
    return {};
}

constexpr const char* kPlatformsFolder = "platforms";
constexpr const char* kPlatformFile = "lumora-platform.json";

} // namespace

EditorInstaller::EditorInstaller(QString installRoot, QObject* parent) : QObject(parent), root_(std::move(installRoot)) {}

EditorInstaller::~EditorInstaller() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
    }
    if (download_) download_->remove();
}

QString EditorInstaller::defaultInstallRoot() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("Editors"));
}

std::vector<InstalledEditor> EditorInstaller::installed() const {
    std::vector<InstalledEditor> out;
    const QDir root(root_);
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.startsWith(QLatin1Char('.'))) continue;  // staging / downloads
        if (auto e = readInstall(root.filePath(name)); e && e->version == name) out.push_back(std::move(*e));
    }
    std::ranges::sort(out, [](const InstalledEditor& a, const InstalledEditor& b) {
        return compareVersions(a.version, b.version) > 0;
    });
    return out;
}

std::optional<InstalledEditor> EditorInstaller::find(const QString& version) const {
    if (!isValidVersion(version)) return std::nullopt;
    if (auto e = readInstall(QDir(root_).filePath(version)); e && e->version == version) return e;
    return std::nullopt;
}

QNetworkReply* EditorInstaller::get(const QUrl& url) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("LumoraHub/%1").arg(QString::fromLatin1(version::kString)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);  // stalled, not slow: the timer restarts with every byte
    return network_.get(request);
}

// ---- Release feed -----------------------------------------------------------------------------

void EditorInstaller::fetchManifest(const QUrl& feedUrl) {
    if (!feedUrl.isValid() || (feedUrl.scheme() != QLatin1String("https") && feedUrl.scheme() != QLatin1String("http") &&
                               feedUrl.scheme() != QLatin1String("file"))) {
        emit manifestFailed(tr("The release feed address '%1' is not a valid http(s) or file URL.").arg(feedUrl.toString()));
        return;
    }
    QNetworkReply* reply = get(feedUrl);
    connect(reply, &QNetworkReply::finished, this, [this, reply, feedUrl] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit manifestFailed(tr("Cannot download the release list: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->read(kMaxFeedBytes + 1);
        if (body.size() > kMaxFeedBytes) {
            emit manifestFailed(tr("The release list is unreasonably large."));
            return;
        }
        if (publicKey_.isEmpty()) {
            finishManifest(body, feedUrl, std::nullopt);
            return;
        }
        QUrl signatureUrl = feedUrl;
        signatureUrl.setPath(feedUrl.path() + QStringLiteral(".sig"));
        QNetworkReply* sig = get(signatureUrl);
        connect(sig, &QNetworkReply::finished, this, [this, sig, body, feedUrl] {
            sig->deleteLater();
            if (sig->error() != QNetworkReply::NoError) {
                emit manifestFailed(tr("The release list is not signed (%1).").arg(sig->errorString()));
                return;
            }
            const QByteArray raw = sig->read(4096);
            const auto decoded = raw.size() == 64 ? std::optional<QByteArray>(raw) : decodeBinary(QString::fromLatin1(raw), 64);
            finishManifest(body, feedUrl, decoded.value_or(QByteArray()));
        });
    });
}

void EditorInstaller::finishManifest(const QByteArray& body, const QUrl& feedUrl, const std::optional<QByteArray>& signature) {
    if (!publicKey_.isEmpty() && (!signature || !verifyEd25519(body, *signature, publicKey_))) {
        log::error("Launcher", "release feed {} has an invalid signature", feedUrl.toString().toStdString());
        emit manifestFailed(tr("The release list's signature is invalid - it was not published by Lumora's release key."));
        return;
    }
    auto manifest = ReleaseManifest::parse(body, feedUrl);
    if (!manifest) {
        emit manifestFailed(QString::fromStdString(manifest.error().message));
        return;
    }
    log::info("Launcher", "release feed: {} releases", manifest->releases.size());
    emit manifestReady(*manifest);
}

// ---- Install ----------------------------------------------------------------------------------

void EditorInstaller::install(const ReleaseInfo& release, const PackageInfo& package) {
    if (busy() || installing_) return emit installFailed(tr("Another installation is in progress."));
    if (!isValidVersion(release.version)) return emit installFailed(tr("Invalid version '%1'.").arg(release.version));
    if (find(release.version)) return emit installFailed(tr("Lumora %1 is already installed.").arg(release.version));
    if (!QDir(root_).mkpath(QStringLiteral(".downloads"))) {
        return emit installFailed(tr("Cannot create %1.").arg(QDir(root_).filePath(QStringLiteral(".downloads"))));
    }
    release_ = release;
    package_ = package;
    installing_ = true;
    cancelled_ = false;
    staging_ = QDir(root_).filePath(QStringLiteral(".staging-") + release.version);
    QDir(staging_).removeRecursively();
    // With an earlier version installed, fetch only the files that changed.
    if (package.supportsDelta() && !installed().empty()) {
        startDelta();
    } else {
        startZip();
    }
}

bool EditorInstaller::busy() const noexcept { return installing_; }

void EditorInstaller::installPlatformSupport(const InstalledEditor& editor, const ReleaseInfo& release,
                                             const PackageInfo& package) {
    if (installing_) return emit installFailed(tr("Another installation is in progress."));
    if (!isValidVersion(release.version) || release.version != editor.version) {
        return emit installFailed(tr("Build support must come from the editor's own release (%1).").arg(editor.version));
    }
    if (package.platform == currentPlatform() || !supportedPlatforms().contains(package.platform)) {
        return emit installFailed(tr("No build support for '%1'.").arg(package.platform));
    }
    if (!QDir(root_).mkpath(QStringLiteral(".downloads"))) {
        return emit installFailed(tr("Cannot create %1.").arg(QDir(root_).filePath(QStringLiteral(".downloads"))));
    }
    release_ = release;
    package_ = package;
    installing_ = true;
    cancelled_ = false;
    delta_ = false;
    supportFor_ = editor.directory;
    staging_ = QDir(root_).filePath(QStringLiteral(".staging-%1-%2").arg(release.version, package.platform));
    QDir(staging_).removeRecursively();
    startZip();
}

QStringList EditorInstaller::platformSupport(const InstalledEditor& editor) {
    QStringList out;
    const QDir platforms(QDir(editor.directory).filePath(QString::fromLatin1(kPlatformsFolder)));
    for (const QString& platform : platforms.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (QFileInfo::exists(platforms.filePath(platform + QLatin1Char('/') + editorExecutable(platform)))) out << platform;
    }
    return out;
}

Result<> EditorInstaller::removePlatformSupport(const InstalledEditor& editor, const QString& platform) {
    if (!supportedPlatforms().contains(platform)) return makeError("unknown platform '{}'", platform.toStdString());
    const QString dir = QDir(editor.directory).filePath(QStringLiteral("%1/%2").arg(QLatin1String(kPlatformsFolder), platform));
    if (!QFileInfo(dir).isDir()) return makeError("{} build support is not installed", platform.toStdString());
    if (!QDir(dir).removeRecursively()) return makeError("could not remove {} completely", dir.toStdString());
    log::info("Launcher", "removed {} build support from Lumora {}", platform.toStdString(), editor.version.toStdString());
    return {};
}

void EditorInstaller::finalizePlatformSupport(const QString& content) {
    const json info = {{"version", release_.version.toStdString()},
                       {"platform", package_.platform.toStdString()},
                       {"source", package_.url.toString().toStdString()},
                       {"sha256", package_.sha256.toStdString()},
                       {"installedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()}};
    const std::string text = info.dump(2);
    QSaveFile record(QDir(content).filePath(QString::fromLatin1(kPlatformFile)));
    if (!record.open(QIODevice::WriteOnly) || record.write(text.data(), static_cast<qint64>(text.size())) < 0 ||
        !record.commit()) {
        return failInstall(tr("Cannot record the installation."));
    }
    const QString platforms = QDir(supportFor_).filePath(QString::fromLatin1(kPlatformsFolder));
    const QString target = QDir(platforms).filePath(package_.platform);
    QDir(target).removeRecursively();
    if (!QDir().mkpath(platforms) || !QDir().rename(content, target)) {
        return failInstall(tr("Cannot move the build support into %1.").arg(target));
    }
    QDir(staging_).removeRecursively();
    log::info("Launcher", "installed {} build support for Lumora {} in {}", package_.platform.toStdString(),
              release_.version.toStdString(), target.toStdString());
    installing_ = false;
    staging_.clear();
    supportFor_.clear();
    emit platformSupportInstalled(release_.version, package_.platform);
}

void EditorInstaller::startZip() {
    download_ = std::make_unique<QFile>(QDir(root_).filePath(
        QStringLiteral(".downloads/lumora-editor-%1-%2.zip.part").arg(release_.version, package_.platform)));
    if (!download_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return failInstall(tr("Cannot write the download: %1").arg(download_->errorString()));
    }
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    received_ = 0;
    log::info("Launcher", "downloading Lumora {} from {}", release_.version.toStdString(),
              package_.url.toString().toStdString());
    streamTo(package_.url, package_.size, [this] { finishDownload(); });
}

void EditorInstaller::streamTo(const QUrl& url, quint64 expected, std::function<void()> done) {
    reply_ = get(url);
    const qint64 base = received_;
    connect(reply_, &QNetworkReply::readyRead, this, [this, expected, base] {
        const QByteArray chunk = reply_->readAll();
        received_ += chunk.size();
        if (static_cast<quint64>(received_ - base) > expected) {
            abortReply();
            return failInstall(tr("The download is larger than announced."));
        }
        hash_->addData(chunk);
        if (download_->write(chunk) != chunk.size()) {
            abortReply();
            return failInstall(tr("Cannot write the download: %1").arg(download_->errorString()));
        }
        emit progress(received_, progressTotal());
    });
    connect(reply_, &QNetworkReply::finished, this, std::move(done));
}

void EditorInstaller::abortReply() {
    if (!reply_) return;
    QNetworkReply* reply = reply_;
    reply_ = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

qint64 EditorInstaller::progressTotal() const {
    return static_cast<qint64>(delta_ ? deltaTotal_ : package_.size);
}

void EditorInstaller::cancel() {
    if (!installing_) return;
    cancelled_ = true;
    if (reply_) reply_->abort();  // finished() follows with OperationCanceledError
}

void EditorInstaller::failInstall(const QString& error) {
    if (download_) {
        download_->close();
        download_->remove();
        download_.reset();
    }
    hash_.reset();
    if (!staging_.isEmpty()) QDir(staging_).removeRecursively();
    installing_ = false;
    delta_ = false;
    supportFor_.clear();
    log::warn("Launcher", "install failed: {}", error.toStdString());
    emit installFailed(error);
}

/// Common end of a finished reply: false (after reporting) if it failed or was cancelled.
bool EditorInstaller::takeReply(QNetworkReply*& reply) {
    reply = reply_;
    reply_ = nullptr;
    if (reply == nullptr) return false;
    reply->deleteLater();
    if (cancelled_ || reply->error() == QNetworkReply::OperationCanceledError) {
        failInstall(tr("Installation cancelled."));
        return false;
    }
    return true;
}

void EditorInstaller::finishDownload() {
    QNetworkReply* reply = nullptr;
    if (!takeReply(reply)) return;
    if (reply->error() != QNetworkReply::NoError) return failInstall(tr("Download failed: %1").arg(reply->errorString()));
    const QByteArray rest = reply->readAll();
    received_ += rest.size();
    hash_->addData(rest);
    download_->write(rest);
    download_->close();
    if (static_cast<quint64>(received_) != package_.size) {
        return failInstall(tr("The download is incomplete (%1 of %2 bytes).").arg(received_).arg(package_.size));
    }
    if (hash_->result().toHex() != package_.sha256) {
        return failInstall(tr("The download is corrupted (checksum mismatch)."));
    }

    // Extract (and index the files for later delta updates) off the UI thread.
    const QString archive = download_->fileName();
    const QString executable = editorExecutable(package_.platform);
    struct Outcome {
        QString error;
        QString content;
        std::vector<FileEntry> entries;
    };
    auto outcome = std::make_shared<Outcome>();
    QThread* worker = QThread::create([archive, staging = staging_, outcome, executable] {
        if (auto extracted = extractZip(archive, staging); !extracted) {
            outcome->error = tr("Cannot unpack the package: %1").arg(QString::fromStdString(extracted.error().message));
            return;
        }
        outcome->content = contentFolder(staging, executable);
        if (outcome->content.isEmpty()) {
            outcome->error = tr("The package does not contain %1.").arg(executable);
            return;
        }
        if (auto scanned = scanFolder(outcome->content)) outcome->entries = std::move(*scanned);
    });
    connect(worker, &QThread::finished, this, [this, worker, archive, outcome] {
        worker->deleteLater();
        QFile::remove(archive);
        download_.reset();
        hash_.reset();
        if (!outcome->error.isEmpty()) return failInstall(outcome->error);
        if (!supportFor_.isEmpty()) return finalizePlatformSupport(outcome->content);
        finalize(outcome->content, outcome->entries);
    });
    worker->start();
}

// ---- Delta updates ----------------------------------------------------------------------------

void EditorInstaller::startDelta() {
    delta_ = true;
    log::info("Launcher", "updating to Lumora {}: fetching the file list", release_.version.toStdString());
    reply_ = get(package_.filesUrl);
    connect(reply_, &QNetworkReply::finished, this, &EditorInstaller::finishFileList);
}

void EditorInstaller::fallBackToZip(const QString& why) {
    log::warn("Launcher", "delta update not possible ({}); downloading the full package", why.toStdString());
    abortReply();
    if (download_) {
        download_->close();
        download_->remove();
        download_.reset();
    }
    QDir(staging_).removeRecursively();
    delta_ = false;
    received_ = 0;
    startZip();
}

void EditorInstaller::finishFileList() {
    QNetworkReply* reply = nullptr;
    if (!takeReply(reply)) return;
    if (reply->error() != QNetworkReply::NoError) return fallBackToZip(reply->errorString());
    const QByteArray body = reply->read(static_cast<qint64>(package_.filesSize) + 1);
    if (static_cast<quint64>(body.size()) != package_.filesSize ||
        QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() != package_.filesSha256) {
        return fallBackToZip(QStringLiteral("file list does not match the feed"));
    }
    auto entries = parseFileList(body);
    if (!entries) return fallBackToZip(QString::fromStdString(entries.error().message));
    entries_ = std::move(*entries);

    // Files already on disk (in installed versions) by hash.
    auto local = std::make_shared<std::map<QByteArray, QString>>();
    for (const InstalledEditor& e : installed()) {
        QFile list(QDir(e.directory).filePath(QString::fromLatin1(kInstalledFileList)));
        if (!list.open(QIODevice::ReadOnly)) continue;
        auto files = parseFileList(list.readAll());
        if (!files) continue;
        for (const FileEntry& f : *files) {
            if (f.link.isEmpty()) local->emplace(f.sha256, QDir(e.directory).filePath(f.path));
        }
    }
    // Copy what can be reused off the UI thread; the rest is downloaded.
    auto missing = std::make_shared<std::vector<std::size_t>>();
    auto error = std::make_shared<QString>();
    QThread* worker = QThread::create([entries = entries_, staging = staging_, local, missing, error] {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const FileEntry& f = entries[i];
            if (!f.link.isEmpty()) continue;
            const QString target = QDir(staging).filePath(f.path);
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
                *error = QStringLiteral("cannot create %1").arg(QFileInfo(target).absolutePath());
                return;
            }
            const auto it = local->find(f.sha256);
            if (it != local->end() && QFileInfo(it->second).size() == static_cast<qint64>(f.size) &&
                QFile::copy(it->second, target)) {
                continue;
            }
            missing->push_back(i);
        }
    });
    connect(worker, &QThread::finished, this, [this, worker, missing, error] {
        worker->deleteLater();
        if (cancelled_) return failInstall(tr("Installation cancelled."));
        if (!error->isEmpty()) return fallBackToZip(*error);
        missing_ = std::move(*missing);
        nextMissing_ = 0;
        deltaTotal_ = 0;
        for (std::size_t i : missing_) deltaTotal_ += static_cast<qint64>(entries_[i].size);
        received_ = 0;
        log::info("Launcher", "update to Lumora {}: {} of {} files changed ({:.1f} MiB to download)",
                  release_.version.toStdString(), missing_.size(), entries_.size(),
                  static_cast<double>(deltaTotal_) / (1024.0 * 1024.0));
        downloadNextBlob();
    });
    worker->start();
}

void EditorInstaller::downloadNextBlob() {
    if (nextMissing_ >= missing_.size()) return finishDelta();
    const FileEntry& f = entries_[missing_[nextMissing_]];
    download_ = std::make_unique<QFile>(QDir(staging_).filePath(f.path));
    if (!download_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fallBackToZip(download_->errorString());
    }
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    blobStart_ = received_;
    streamTo(package_.blobsUrl.resolved(QUrl(QString::fromLatin1(f.sha256))), f.size, [this] { finishBlob(); });
}

void EditorInstaller::finishBlob() {
    QNetworkReply* reply = nullptr;
    if (!takeReply(reply)) return;
    if (reply->error() != QNetworkReply::NoError) return fallBackToZip(reply->errorString());
    const QByteArray rest = reply->readAll();
    received_ += rest.size();
    hash_->addData(rest);
    download_->write(rest);
    download_->close();
    const FileEntry& f = entries_[missing_[nextMissing_]];
    if (static_cast<quint64>(received_ - blobStart_) != f.size || hash_->result().toHex() != f.sha256) {
        return fallBackToZip(QStringLiteral("%1 does not match its checksum").arg(f.path));
    }
    download_.reset();
    hash_.reset();
    ++nextMissing_;
    downloadNextBlob();
}

void EditorInstaller::finishDelta() {
    for (const FileEntry& f : entries_) {
        const QString target = QDir(staging_).filePath(f.path);
        if (!f.link.isEmpty()) {
#if !defined(Q_OS_WIN)
            QDir().mkpath(QFileInfo(target).absolutePath());
            std::error_code ec;
            std::filesystem::create_symlink(f.link.toStdString(), target.toStdString(), ec);
            if (ec) return fallBackToZip(QString::fromStdString(ec.message()));
#endif
            continue;
        }
#if !defined(Q_OS_WIN)
        QFile::setPermissions(target, (f.mode & 0100) ? QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                             QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                                             QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                             QFileDevice::ExeOther
                                                       : QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                             QFileDevice::ReadGroup | QFileDevice::ReadOther);
#endif
    }
    const QString content = contentFolder(staging_);
    if (content.isEmpty()) return fallBackToZip(tr("the file list has no %1").arg(editorExecutable()));
    finalize(content, entries_);
}

void EditorInstaller::finalize(const QString& content, const std::vector<FileEntry>& entries) {
    const json info = {{"version", release_.version.toStdString()},
                       {"executable", editorExecutable().toStdString()},
                       {"source", package_.url.toString().toStdString()},
                       {"sha256", package_.sha256.toStdString()},
                       {"update", delta_ ? "delta" : "full"},
                       {"installedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()}};
    const std::string text = info.dump(2);
    QSaveFile record(QDir(content).filePath(QString::fromLatin1(kInstallFile)));
    QSaveFile list(QDir(content).filePath(QString::fromLatin1(kInstalledFileList)));
    const QByteArray listing = fileListJson(entries);
    if (!record.open(QIODevice::WriteOnly) || record.write(text.data(), static_cast<qint64>(text.size())) < 0 ||
        !record.commit() || !list.open(QIODevice::WriteOnly) || list.write(listing) < 0 || !list.commit()) {
        return failInstall(tr("Cannot record the installation."));
    }
    const QString target = QDir(root_).filePath(release_.version);
    if (!QDir().rename(content, target)) return failInstall(tr("Cannot move the editor into %1.").arg(target));
    QDir(staging_).removeRecursively();  // empty wrapper folder, if any
    log::info("Launcher", "installed Lumora {} ({}, {} files) in {}", release_.version.toStdString(),
              delta_ ? "delta update" : "full package", entries.size(), target.toStdString());
    const bool wasDelta = delta_;
    installing_ = false;
    delta_ = false;
    staging_.clear();
    lastWasDelta_ = wasDelta;
    if (auto editor = find(release_.version)) {
        emit installCompleted(*editor);
    } else {
        failInstall(tr("The installed editor could not be found."));
    }
}

Result<> EditorInstaller::uninstall(const QString& version) {
    const auto editor = find(version);
    if (!editor) return makeError("Lumora {} is not installed", version.toStdString());
    if (!QDir(editor->directory).removeRecursively()) {
        return makeError("could not remove {} completely", editor->directory.toStdString());
    }
    log::info("Launcher", "removed Lumora {}", version.toStdString());
    return {};
}

} // namespace lumora::launcher
