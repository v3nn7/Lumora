#include "common/Preferences.hpp"

#include "common/ProjectUi.hpp"
#include "platform/Paths.hpp"

#include <QSettings>

namespace lumora::app {

Preferences Preferences::load() {
    QSettings s;
    Preferences p;
    const int gpu = s.value(QStringLiteral("preferences/gpu"), -1).toInt();
    if (gpu >= 0) p.gpuIndex = static_cast<std::uint32_t>(gpu);
    const int validation = s.value(QStringLiteral("preferences/validation"), 0).toInt();
    p.validation = validation >= 0 && validation <= 2 ? static_cast<Validation>(validation) : Validation::Automatic;
    p.vsync = s.value(QStringLiteral("preferences/vsync"), true).toBool();
    p.backend = s.value(QStringLiteral("preferences/backend"), QStringLiteral("vulkan")).toString();
    p.projectsDirectory = s.value(QStringLiteral("preferences/projectsDirectory")).toString();
    return p;
}

void Preferences::save() const {
    QSettings s;
    s.setValue(QStringLiteral("preferences/gpu"), gpuIndex ? static_cast<int>(*gpuIndex) : -1);
    s.setValue(QStringLiteral("preferences/validation"), static_cast<int>(validation));
    s.setValue(QStringLiteral("preferences/vsync"), vsync);
    s.setValue(QStringLiteral("preferences/backend"), backend);
    s.setValue(QStringLiteral("preferences/projectsDirectory"), projectsDirectory);
}

void Preferences::applyTo(AppOptions& options) const {
    if (!options.gpuIndex) options.gpuIndex = gpuIndex;
    if (!options.validation && validation != Validation::Automatic) options.validation = validation == Validation::On;
    if (!options.vsync) options.vsync = vsync;
    if (options.backend.empty() && !backend.isEmpty()) options.backend = backend.toStdString();
}

QString Preferences::effectiveProjectsDirectory() const {
    return projectsDirectory.isEmpty() ? toQString(paths::defaultProjectsDir()) : projectsDirectory;
}

} // namespace lumora::app
