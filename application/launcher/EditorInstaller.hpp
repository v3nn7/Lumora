#pragma once

#include "common/ModuleExport.hpp"

#include "core/Error.hpp"
#include "launcher/FileList.hpp"
#include "launcher/ReleaseManifest.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lumora::launcher {

struct InstalledEditor {
    QString version;
    QString directory;   // absolute install folder
    QString executable;  // absolute path of the editor binary
    QString source;      // package URL it came from
    QDateTime installedAt;
};

/// Installs editor versions next to each other, Unity-Hub style:
///   <root>/<version>/bin/lumora-editor(.exe), lib/..., shaders/..., lumora-install.json
///
/// The release feed is fetched over HTTPS (or file:// for local feeds) and, when the build has
/// a release key, must carry a valid Ed25519 signature. Packages are streamed to disk, checked
/// against the size and SHA-256 in the (signed) feed, extracted into a staging folder and only
/// then moved into place, so a failed or cancelled install never leaves a half-installed version.
///
/// Updates: when a version is already installed and the feed lists the package's files, only
/// files whose SHA-256 no installed version has are downloaded (from <blobs>/<sha256>); the
/// rest are copied locally. Any problem on that path falls back to the full package.
class LUMORA_LAUNCHER_API EditorInstaller final : public QObject {
    Q_OBJECT
public:
    explicit EditorInstaller(QString installRoot, QObject* parent = nullptr);
    ~EditorInstaller() override;

    /// Default install root: <AppLocalData>/Editors.
    [[nodiscard]] static QString defaultInstallRoot();

    [[nodiscard]] const QString& installRoot() const noexcept { return root_; }
    /// Ed25519 key the feed must be signed with; empty = unsigned feeds are accepted.
    void setPublicKey(QByteArray key) { publicKey_ = std::move(key); }

    [[nodiscard]] std::vector<InstalledEditor> installed() const;  // newest first
    [[nodiscard]] std::optional<InstalledEditor> find(const QString& version) const;

    void fetchManifest(const QUrl& feedUrl);
    void install(const ReleaseInfo& release, const PackageInfo& package);
    void cancel();
    [[nodiscard]] bool busy() const noexcept;
    /// The last completed install downloaded only changed files (testing and diagnostics).
    [[nodiscard]] bool lastInstallWasDelta() const noexcept { return lastWasDelta_; }
    Result<> uninstall(const QString& version);

signals:
    void manifestReady(const lumora::launcher::ReleaseManifest& manifest);
    void manifestFailed(const QString& error);
    void progress(qint64 received, qint64 total);
    void installCompleted(const lumora::launcher::InstalledEditor& editor);
    void installFailed(const QString& error);

private:
    void finishManifest(const QByteArray& body, const QUrl& feedUrl, const std::optional<QByteArray>& signature);
    void startZip();
    void finishDownload();
    void startDelta();
    void finishFileList();
    void downloadNextBlob();
    void finishBlob();
    void finishDelta();
    void fallBackToZip(const QString& why);
    void finalize(const QString& content, const std::vector<FileEntry>& entries);
    void streamTo(const QUrl& url, quint64 expected, std::function<void()> done);
    void abortReply();
    bool takeReply(QNetworkReply*& reply);
    [[nodiscard]] qint64 progressTotal() const;
    void failInstall(const QString& error);
    [[nodiscard]] QNetworkReply* get(const QUrl& url);

    QString root_;
    QByteArray publicKey_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    // Download in progress.
    ReleaseInfo release_;
    PackageInfo package_;
    std::unique_ptr<QFile> download_;
    std::unique_ptr<QCryptographicHash> hash_;
    qint64 received_ = 0;
    bool installing_ = false;
    bool cancelled_ = false;
    QString staging_;
    // Delta update in progress.
    bool delta_ = false;
    bool lastWasDelta_ = false;
    std::vector<FileEntry> entries_;
    std::vector<std::size_t> missing_;  // indices into entries_ still to download
    std::size_t nextMissing_ = 0;
    qint64 deltaTotal_ = 0;
    qint64 blobStart_ = 0;
};

} // namespace lumora::launcher
