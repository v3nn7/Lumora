#include "launcher/HubUpdate.hpp"

#include <algorithm>

namespace lumora::launcher {

std::optional<QString> hubUpdateAvailable(const ReleaseManifest& manifest, const QString& currentVersion,
                                          Channel preferred, const QString& platform, const QString& skippedVersion) {
    if (!isValidVersion(currentVersion)) return std::nullopt;
    const Channel upTo = std::max(preferred, channelOfVersion(currentVersion));
    const ReleaseInfo* newest = manifest.latest(platform, upTo);
    if (newest == nullptr || compareVersions(newest->version, currentVersion) <= 0) return std::nullopt;
    if (newest->version == skippedVersion) return std::nullopt;
    return newest->version;
}

} // namespace lumora::launcher
