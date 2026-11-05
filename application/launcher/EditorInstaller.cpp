#include "launcher/EditorInstaller.hpp"

#include "core/Log.hpp"
#include "core/Version.hpp"
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
QString contentFolder(const QString& staging) {
    if (QFileInfo::exists(QDir(staging).filePath(editorExecutable()))) return staging;
    const QStringList entries = QDir(staging).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    if (entries.size() == 1) {
        const QString inner = QDir(staging).filePath(entries.first());
        if (QFileInfo::exists(QDir(inner).filePath(editorExecutable()))) return inner;
    }
    return {};
}

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
    if (busy()) return emit installFailed(tr("Another installation is in progress."));
    if (!isValidVersion(release.version)) return emit installFailed(tr("Invalid version '%1'.").arg(release.version));
    if (find(release.version)) return emit installFailed(tr("Lumora %1 is already installed.").arg(release.version));
    const QDir root(root_);
    if (!root.mkpath(QStringLiteral(".downloads"))) {
        return emit installFailed(tr("Cannot create %1.").arg(root.filePath(QStringLiteral(".downloads"))));
    }
    release_ = release;
    package_ = package;
    download_ = std::make_unique<QFile>(
        root.filePath(QStringLiteral(".downloads/lumora-editor-%1-%2.zip.part").arg(release.version, package.platform)));
    if (!download_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = download_->errorString();
        download_.reset();
        return emit installFailed(tr("Cannot write the download: %1").arg(why));
    }
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    received_ = 0;
    log::info("Launcher", "downloading Lumora {} from {}", release.version.toStdString(), package.url.toString().toStdString());
    reply_ = get(package.url);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        const QByteArray chunk = reply_->readAll();
        received_ += chunk.size();
        if (static_cast<quint64>(received_) > package_.size) {
            reply_->disconnect(this);
            reply_->abort();
            reply_->deleteLater();
            reply_ = nullptr;
            return failInstall(tr("The download is larger than announced."));
        }
        hash_->addData(chunk);
        if (download_->write(chunk) != chunk.size()) {
            reply_->disconnect(this);
            reply_->abort();
            reply_->deleteLater();
            reply_ = nullptr;
            return failInstall(tr("Cannot write the download: %1").arg(download_->errorString()));
        }
        emit progress(received_, static_cast<qint64>(package_.size));
    });
    connect(reply_, &QNetworkReply::finished, this, &EditorInstaller::finishDownload);
}

void EditorInstaller::cancel() {
    if (!reply_) return;
    reply_->abort();  // finished() follows with OperationCanceledError
}

void EditorInstaller::failInstall(const QString& error) {
    if (download_) {
        download_->close();
        download_->remove();
        download_.reset();
    }
    hash_.reset();
    log::warn("Launcher", "install failed: {}", error.toStdString());
    emit installFailed(error);
}

void EditorInstaller::finishDownload() {
    QNetworkReply* reply = reply_;
    reply_ = nullptr;
    if (reply == nullptr) return;
    reply->deleteLater();
    if (reply->error() == QNetworkReply::OperationCanceledError) return failInstall(tr("Installation cancelled."));
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

    // Extract off the UI thread, then move into place here.
    const QString archive = download_->fileName();
    const QString staging = QDir(root_).filePath(QStringLiteral(".staging-") + release_.version);
    QDir(staging).removeRecursively();
    auto result = std::make_shared<Result<ExtractStats>>(makeError("not run"));
    QThread* worker = QThread::create([archive, staging, result] { *result = extractZip(archive, staging); });
    connect(worker, &QThread::finished, this, [this, worker, archive, staging, result] {
        worker->deleteLater();
        QFile::remove(archive);
        download_.reset();
        hash_.reset();
        if (!*result) {
            QDir(staging).removeRecursively();
            return failInstall(tr("Cannot unpack the package: %1").arg(QString::fromStdString(result->error().message)));
        }
        const QString content = contentFolder(staging);
        if (content.isEmpty()) {
            QDir(staging).removeRecursively();
            return failInstall(tr("The package does not contain %1.").arg(editorExecutable()));
        }
        const json info = {{"version", release_.version.toStdString()},
                           {"executable", editorExecutable().toStdString()},
                           {"source", package_.url.toString().toStdString()},
                           {"sha256", package_.sha256.toStdString()},
                           {"installedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()}};
        QSaveFile file(QDir(content).filePath(QString::fromLatin1(kInstallFile)));
        const std::string text = info.dump(2);
        if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), static_cast<qint64>(text.size())) < 0 ||
            !file.commit()) {
            QDir(staging).removeRecursively();
            return failInstall(tr("Cannot record the installation."));
        }
        const QString target = QDir(root_).filePath(release_.version);
        if (!QDir().rename(content, target)) {
            QDir(staging).removeRecursively();
            return failInstall(tr("Cannot move the editor into %1.").arg(target));
        }
        QDir(staging).removeRecursively();  // empty wrapper folder, if any
        log::info("Launcher", "installed Lumora {} ({} files, {:.1f} MiB) in {}", release_.version.toStdString(),
                  result->value().files, static_cast<double>(result->value().bytes) / (1024.0 * 1024.0),
                  target.toStdString());
        if (auto editor = find(release_.version)) {
            emit installCompleted(*editor);
        } else {
            failInstall(tr("The installed editor could not be found."));
        }
    });
    worker->start();
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
