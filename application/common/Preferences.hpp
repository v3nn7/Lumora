#pragma once

#include "main/CommandLine.hpp"

#include <QString>

#include <cstdint>
#include <optional>

namespace lumora::app {

/// User preferences persisted with QSettings (Hub > Settings). Command-line flags override them.
struct Preferences {
    enum class Validation : int { Automatic = 0, On = 1, Off = 2 };

    std::optional<std::uint32_t> gpuIndex;  // nullopt = pick the best suitable GPU
    QString backend;                        // "vulkan" (default) or "d3d12" (Windows builds with it)
    Validation validation = Validation::Automatic;
    bool vsync = true;
    QString projectsDirectory;              // empty = platform default

    [[nodiscard]] static Preferences load();
    void save() const;

    /// Fills options the command line left unset.
    void applyTo(AppOptions& options) const;
    /// Default location for new projects (preference or platform default).
    [[nodiscard]] QString effectiveProjectsDirectory() const;
};

} // namespace lumora::app
