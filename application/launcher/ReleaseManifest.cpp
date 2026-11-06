#include "launcher/ReleaseManifest.hpp"

#include <nlohmann/json.hpp>
#include <QRegularExpression>
#include <QStringList>
#include <QSysInfo>

#include <algorithm>

namespace lumora::launcher {

using nlohmann::json;

QString currentPlatform() {
#if defined(Q_OS_WIN)
    const QString os = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    const QString os = QStringLiteral("macos");
#else
    const QString os = QStringLiteral("linux");
#endif
#if defined(Q_PROCESSOR_ARM_64)
    return os + QStringLiteral("-arm64");
#else
    return os + QStringLiteral("-x64");
#endif
}

QString editorExecutable() {
#if defined(Q_OS_WIN)
    return QStringLiteral("bin/lumora-editor.exe");
#else
    return QStringLiteral("bin/lumora-editor");
#endif
}

QString editorExecutable(const QString& platform) {
    return platform.startsWith(QLatin1String("windows")) ? QStringLiteral("bin/lumora-editor.exe")
                                                          : QStringLiteral("bin/lumora-editor");
}

QStringList supportedPlatforms() { return {QStringLiteral("linux-x64"), QStringLiteral("windows-x64")}; }

QString platformTitle(const QString& platform) {
    if (platform.startsWith(QLatin1String("windows"))) return QStringLiteral("Windows");
    if (platform.startsWith(QLatin1String("linux"))) return QStringLiteral("Linux");
    if (platform.startsWith(QLatin1String("macos"))) return QStringLiteral("macOS");
    return platform;
}

bool isValidVersion(const QString& version) {
    static const QRegularExpression pattern(QStringLiteral(R"(^\d{1,6}\.\d{1,6}\.\d{1,6}(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?$)"));
    return version.size() <= 64 && pattern.match(version).hasMatch();
}

namespace {

int compareIdentifiers(const QStringList& a, const QStringList& b) {
    // Semantic-versioning precedence for pre-release identifiers.
    for (qsizetype i = 0; i < std::min(a.size(), b.size()); ++i) {
        bool aNum = false;
        bool bNum = false;
        const qulonglong an = a[i].toULongLong(&aNum);
        const qulonglong bn = b[i].toULongLong(&bNum);
        if (aNum && bNum) {
            if (an != bn) return an < bn ? -1 : 1;
        } else if (aNum != bNum) {
            return aNum ? -1 : 1;  // numeric identifiers sort before alphanumeric ones
        } else if (const int c = QString::compare(a[i], b[i]); c != 0) {
            return c < 0 ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

} // namespace

int compareVersions(const QString& a, const QString& b) {
    const QString aCore = a.section(QLatin1Char('-'), 0, 0);
    const QString bCore = b.section(QLatin1Char('-'), 0, 0);
    const QStringList aParts = aCore.split(QLatin1Char('.'));
    const QStringList bParts = bCore.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < std::max(aParts.size(), bParts.size()); ++i) {
        const qulonglong x = i < aParts.size() ? aParts[i].toULongLong() : 0;
        const qulonglong y = i < bParts.size() ? bParts[i].toULongLong() : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    const QString aPre = a.section(QLatin1Char('-'), 1);
    const QString bPre = b.section(QLatin1Char('-'), 1);
    if (aPre.isEmpty() || bPre.isEmpty()) return aPre.isEmpty() == bPre.isEmpty() ? 0 : (aPre.isEmpty() ? 1 : -1);
    return compareIdentifiers(aPre.split(QLatin1Char('.')), bPre.split(QLatin1Char('.')));
}

Channel channelFromName(const QString& name) {
    if (name == QLatin1String("stable")) return Channel::Stable;
    if (name == QLatin1String("beta")) return Channel::Beta;
    return Channel::Alpha;
}

QString channelName(Channel channel) {
    switch (channel) {
        case Channel::Stable:
            return QStringLiteral("stable");
        case Channel::Beta:
            return QStringLiteral("beta");
        case Channel::Alpha:
            break;
    }
    return QStringLiteral("alpha");
}

Channel channelOfVersion(const QString& version) {
    const QString suffix = version.section(QLatin1Char('-'), 1);
    if (suffix.isEmpty()) return Channel::Stable;
    if (suffix.startsWith(QLatin1String("beta")) || suffix.startsWith(QLatin1String("rc"))) return Channel::Beta;
    return Channel::Alpha;
}

const PackageInfo* ReleaseInfo::packageFor(const QString& platform) const {
    const auto it = std::ranges::find(packages, platform, &PackageInfo::platform);
    return it == packages.end() ? nullptr : &*it;
}

Result<ReleaseManifest> ReleaseManifest::parse(const QByteArray& bytes, const QUrl& feedUrl) {
    const json root = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) return makeError("the release feed is not valid JSON");
    if (root.value("format", std::string{}) != "lumora.releases") return makeError("not a Lumora release feed");
    if (root.value("version", 0) != 1) return makeError("unsupported release feed version {}", root.value("version", 0));
    const auto list = root.find("releases");
    if (list == root.end() || !list->is_array()) return makeError("the release feed has no 'releases' list");

    static const QRegularExpression hexSha(QStringLiteral("^[0-9a-f]{64}$"));
    const QUrl blobs = feedUrl.resolved(QUrl(QString::fromStdString(root.value("blobs", std::string("blobs/")))));
    ReleaseManifest manifest;
    for (const json& r : *list) {
        if (!r.is_object()) return makeError("release entry is not an object");
        ReleaseInfo release;
        release.version = QString::fromStdString(r.value("version", std::string{}));
        if (!isValidVersion(release.version)) {
            return makeError("invalid release version '{}'", release.version.toStdString());
        }
        release.channel = QString::fromStdString(r.value("channel", std::string("stable")));
        release.date = QString::fromStdString(r.value("date", std::string{}));
        release.notes = QString::fromStdString(r.value("notes", std::string{}));
        const auto packages = r.find("packages");
        if (packages == r.end() || !packages->is_object()) {
            return makeError("release {} has no packages", release.version.toStdString());
        }
        for (const auto& [platform, p] : packages->items()) {
            if (!p.is_object()) return makeError("package {} of {} is malformed", platform, release.version.toStdString());
            PackageInfo package;
            package.platform = QString::fromStdString(platform);
            const QUrl url(QString::fromStdString(p.value("url", std::string{})));
            package.url = feedUrl.resolved(url);
            package.size = p.value("size", std::uint64_t{0});
            package.sha256 = QByteArray::fromStdString(p.value("sha256", std::string{})).toLower();
            if (!package.url.isValid() || package.url.isEmpty() || package.size == 0 ||
                !hexSha.match(QString::fromLatin1(package.sha256)).hasMatch()) {
                return makeError("package {} of {} needs url, size and sha256", platform, release.version.toStdString());
            }
            if (const auto files = p.find("files"); files != p.end() && files->is_object()) {
                package.filesUrl = feedUrl.resolved(QUrl(QString::fromStdString(files->value("url", std::string{}))));
                package.filesSize = files->value("size", std::uint64_t{0});
                package.filesSha256 = QByteArray::fromStdString(files->value("sha256", std::string{})).toLower();
                package.blobsUrl = blobs;
                if (package.filesSize == 0 || !hexSha.match(QString::fromLatin1(package.filesSha256)).hasMatch()) {
                    package.filesUrl = QUrl();  // malformed: fall back to the full package
                }
            }
            release.packages.push_back(std::move(package));
        }
        manifest.releases.push_back(std::move(release));
    }
    std::ranges::sort(manifest.releases, [](const ReleaseInfo& a, const ReleaseInfo& b) {
        return compareVersions(a.version, b.version) > 0;
    });
    return manifest;
}

const ReleaseInfo* ReleaseManifest::latest(const QString& platform, Channel upTo) const {
    const auto list = visible(platform, upTo);
    return list.empty() ? nullptr : list.front();
}

std::vector<const ReleaseInfo*> ReleaseManifest::visible(const QString& platform, Channel upTo) const {
    std::vector<const ReleaseInfo*> out;
    for (const ReleaseInfo& r : releases) {  // already newest first
        if (channelFromName(r.channel) <= upTo && r.packageFor(platform) != nullptr) out.push_back(&r);
    }
    return out;
}

const ReleaseInfo* ReleaseManifest::find(const QString& version) const {
    const auto it = std::ranges::find(releases, version, &ReleaseInfo::version);
    return it == releases.end() ? nullptr : &*it;
}

} // namespace lumora::launcher
