#pragma once

#include "launcher/ReleaseManifest.hpp"

#include <QString>

#include <optional>

namespace lumora::launcher {

/// Every release in the feed ships the editor and the Hub of the same version, so a newer
/// release for this platform means a newer Hub. The Hub follows the channel picked on its
/// Editors page, and never less than the channel of its own version (an alpha Hub hears about
/// newer alphas). Returns the version to offer, or nothing when the Hub is current or the user
/// chose to skip exactly that version.
[[nodiscard]] std::optional<QString> hubUpdateAvailable(const ReleaseManifest& manifest, const QString& currentVersion,
                                                       Channel preferred, const QString& platform,
                                                       const QString& skippedVersion);

} // namespace lumora::launcher
