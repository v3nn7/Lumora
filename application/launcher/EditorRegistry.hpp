#pragma once

#include "core/Error.hpp"
#include "launcher/EditorInstaller.hpp"

#include <QStringList>

#include <optional>
#include <vector>

namespace lumora::launcher {

/// Every editor the Hub can launch: versions installed from the release feed plus local builds
/// registered by hand (developers pointing the Hub at build/release/bin/lumora-editor).
class EditorRegistry {
public:
    explicit EditorRegistry(QString installRoot = EditorInstaller::defaultInstallRoot());

    [[nodiscard]] std::vector<InstalledEditor> installed() const;   // from the feed, newest first
    [[nodiscard]] std::vector<InstalledEditor> localBuilds() const;  // registered builds
    [[nodiscard]] std::vector<InstalledEditor> all() const;          // both, newest first

    /// Registers an editor executable after asking it for its version (`--version`).
    Result<InstalledEditor> addLocalBuild(const QString& executable);
    void removeLocalBuild(const QString& executable);

    /// The editor for a project made with `projectVersion`: that exact version if available,
    /// else nullopt (the caller then offers the newest one or an install).
    [[nodiscard]] std::optional<InstalledEditor> exactMatch(const QString& projectVersion) const;

    /// Least stable channel the Hub offers (setting; stable by default).
    [[nodiscard]] static Channel preferredChannel();
    static void setPreferredChannel(Channel channel);

    /// The release feed this build was configured with (LUMORA_RELEASE_FEED). Not user-editable:
    /// a Hub only installs editors from the feed it was built for.
    [[nodiscard]] static QString feedUrl();

    /// Arguments that make an editor open a project / join a session.
    [[nodiscard]] static QStringList openProjectArguments(const QString& projectPath);

private:
    QString root_;
};

/// Parses "Lumora 0.4.0 (Release)" (the editor's --version output) into "0.4.0".
[[nodiscard]] std::optional<QString> versionFromBanner(const QString& output);

} // namespace lumora::launcher
