#pragma once

#include "core/Error.hpp"
#include "launcher/ReleaseManifest.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>

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
class EditorInstaller final : public QObject {
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
    [[nodiscard]] bool busy() const noexcept { return reply_ != nullptr; }
    Result<> uninstall(const QString& version);

signals:
    void manifestReady(const lumora::launcher::ReleaseManifest& manifest);
    void manifestFailed(const QString& error);
    void progress(qint64 received, qint64 total);
    void installCompleted(const lumora::launcher::InstalledEditor& editor);
    void installFailed(const QString& error);

private:
    void finishManifest(const QByteArray& body, const QUrl& feedUrl, const std::optional<QByteArray>& signature);
    void finishDownload();
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
};

} // namespace lumora::launcher
