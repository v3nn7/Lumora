#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <vector>

namespace lumora::launcher {

/// Package key of the machine this runs on, e.g. "linux-x64" or "windows-x64".
[[nodiscard]] QString currentPlatform();
/// Editor executable inside an installed package ("bin/lumora-editor[.exe]").
[[nodiscard]] QString editorExecutable();
/// The same inside another platform's package ("windows-x64" -> "bin/lumora-editor.exe").
[[nodiscard]] QString editorExecutable(const QString& platform);
/// Platforms editors are published for, e.g. {"linux-x64", "windows-x64"}.
[[nodiscard]] QStringList supportedPlatforms();
/// "Linux", "Windows" for a package key.
[[nodiscard]] QString platformTitle(const QString& platform);

/// Versions are "MAJOR.MINOR.PATCH" with an optional pre-release suffix ("0.5.0-beta.2").
/// They also name install folders, so anything else is rejected.
[[nodiscard]] bool isValidVersion(const QString& version);
/// <0, 0, >0 like strcmp. Numeric parts compare numerically; a pre-release sorts before its
/// release ("1.0.0-rc.1" < "1.0.0").
[[nodiscard]] int compareVersions(const QString& a, const QString& b);

/// Release channels, from the most to the least stable. A Hub showing a channel also shows the
/// more stable ones.
enum class Channel { Stable = 0, Beta = 1, Alpha = 2 };
/// "stable", "beta", "alpha"; anything unknown counts as the least stable.
[[nodiscard]] Channel channelFromName(const QString& name);
[[nodiscard]] QString channelName(Channel channel);
/// Channel a version belongs to by its suffix: "-alpha*" alpha, "-beta*"/"-rc*" beta, none stable.
[[nodiscard]] Channel channelOfVersion(const QString& version);

struct PackageInfo {
    QString platform;
    QUrl url;            // absolute (relative entries are resolved against the feed)
    quint64 size = 0;
    QByteArray sha256;   // lower-case hex
    // Delta updates (optional): the package's file list and where its files live, by SHA-256.
    QUrl filesUrl;
    quint64 filesSize = 0;
    QByteArray filesSha256;
    QUrl blobsUrl;       // <blobsUrl>/<sha256> is one file of the package
    [[nodiscard]] bool supportsDelta() const noexcept { return filesUrl.isValid() && !filesUrl.isEmpty(); }
};

struct ReleaseInfo {
    QString version;
    QString channel;     // "stable", "beta", ...
    QString date;
    QString notes;
    std::vector<PackageInfo> packages;

    [[nodiscard]] const PackageInfo* packageFor(const QString& platform) const;
};

/// The release feed the Hub reads (manifest.json, published by CI next to the packages):
///
///   {"format": "lumora.releases", "version": 1,
///    "releases": [{"version": "0.4.0", "channel": "stable", "date": "2026-11-05", "notes": "...",
///                  "packages": {"linux-x64": {"url": "editor/0.4.0/lumora-editor-0.4.0-linux-x64.zip",
///                                             "size": 123, "sha256": "..."}}}]}
///
/// A detached Ed25519 signature of the exact bytes lives in manifest.json.sig; package hashes
/// are covered by it.
struct ReleaseManifest {
    std::vector<ReleaseInfo> releases;  // newest first

    [[nodiscard]] static Result<ReleaseManifest> parse(const QByteArray& json, const QUrl& feedUrl);
    /// Newest release for the platform in `upTo` or a more stable channel.
    [[nodiscard]] const ReleaseInfo* latest(const QString& platform, Channel upTo = Channel::Alpha) const;
    /// Releases for the platform in `upTo` or a more stable channel, newest first.
    [[nodiscard]] std::vector<const ReleaseInfo*> visible(const QString& platform, Channel upTo) const;
    [[nodiscard]] const ReleaseInfo* find(const QString& version) const;
};

} // namespace lumora::launcher
