#include "launcher/EditorRegistry.hpp"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>

namespace lumora::launcher {

namespace {

constexpr const char* kLocalKey = "hub/localEditors";  // list of "version|path"
constexpr const char* kChannelKey = "hub/channel";

void sortNewestFirst(std::vector<InstalledEditor>& editors) {
    std::ranges::stable_sort(editors, [](const InstalledEditor& a, const InstalledEditor& b) {
        return compareVersions(a.version, b.version) > 0;
    });
}

} // namespace

std::optional<QString> versionFromBanner(const QString& output) {
    static const QRegularExpression banner(QStringLiteral(R"(Lumora(?:\s+Engine)?\s+(\d+\.\d+\.\d+\S*))"));
    const auto match = banner.match(output);
    if (!match.hasMatch() || !isValidVersion(match.captured(1))) return std::nullopt;
    return match.captured(1);
}

EditorRegistry::EditorRegistry(QString installRoot) : root_(std::move(installRoot)) {}

std::vector<InstalledEditor> EditorRegistry::installed() const { return EditorInstaller(root_).installed(); }

std::vector<InstalledEditor> EditorRegistry::localBuilds() const {
    std::vector<InstalledEditor> out;
    for (const QString& entry : QSettings().value(QString::fromLatin1(kLocalKey)).toStringList()) {
        const QString version = entry.section(QLatin1Char('|'), 0, 0);
        const QString path = entry.section(QLatin1Char('|'), 1);
        if (!isValidVersion(version) || !QFileInfo(path).isFile()) continue;  // moved or deleted build
        InstalledEditor e;
        e.version = version;
        e.executable = path;
        e.directory = QFileInfo(path).absolutePath();
        e.source = QStringLiteral("local");
        out.push_back(std::move(e));
    }
    sortNewestFirst(out);
    return out;
}

std::vector<InstalledEditor> EditorRegistry::all() const {
    std::vector<InstalledEditor> out = installed();
    for (InstalledEditor& e : localBuilds()) out.push_back(std::move(e));
    sortNewestFirst(out);
    return out;
}

Result<InstalledEditor> EditorRegistry::addLocalBuild(const QString& executable) {
    const QFileInfo info(executable);
    if (!info.isFile() || !info.isExecutable()) return makeError("{} is not an executable", executable.toStdString());
    QProcess probe;
    probe.start(info.absoluteFilePath(), {QStringLiteral("--version")});
    if (!probe.waitForFinished(15000)) {
        probe.kill();
        return makeError("{} did not answer --version", executable.toStdString());
    }
    const auto version = versionFromBanner(QString::fromUtf8(probe.readAllStandardOutput()));
    if (!version) return makeError("{} is not a Lumora editor", executable.toStdString());

    QStringList entries = QSettings().value(QString::fromLatin1(kLocalKey)).toStringList();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const QString& e) { return e.section(QLatin1Char('|'), 1) == info.absoluteFilePath(); }),
                  entries.end());
    entries << *version + QLatin1Char('|') + info.absoluteFilePath();
    QSettings().setValue(QString::fromLatin1(kLocalKey), entries);

    InstalledEditor e;
    e.version = *version;
    e.executable = info.absoluteFilePath();
    e.directory = info.absolutePath();
    e.source = QStringLiteral("local");
    return e;
}

void EditorRegistry::removeLocalBuild(const QString& executable) {
    QStringList entries = QSettings().value(QString::fromLatin1(kLocalKey)).toStringList();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const QString& e) { return e.section(QLatin1Char('|'), 1) == executable; }),
                  entries.end());
    QSettings().setValue(QString::fromLatin1(kLocalKey), entries);
}

std::optional<InstalledEditor> EditorRegistry::exactMatch(const QString& projectVersion) const {
    for (const InstalledEditor& e : all()) {
        if (e.version == projectVersion) return e;
    }
    return std::nullopt;
}

Channel EditorRegistry::preferredChannel() {
    const QString name = QSettings().value(QString::fromLatin1(kChannelKey), QStringLiteral("stable")).toString();
    return channelFromName(name);
}

void EditorRegistry::setPreferredChannel(Channel channel) {
    QSettings().setValue(QString::fromLatin1(kChannelKey), channelName(channel));
}

QString EditorRegistry::feedUrl() {
#if defined(LUMORA_RELEASE_FEED)
    return QStringLiteral(LUMORA_RELEASE_FEED);
#else
    return {};
#endif
}

QStringList EditorRegistry::openProjectArguments(const QString& projectPath) {
    return {QStringLiteral("--editor"), projectPath};
}

} // namespace lumora::launcher
