// Lumora: single executable hosting the Hub, the Editor, the standalone Player and headless mode.

#include "collab/CollabUi.hpp"
#include "common/AppContext.hpp"
#include "common/Preferences.hpp"
#include "common/Theme.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include "editor/EditorWindow.hpp"
#include "main/Application.hpp"
#include "main/CommandLine.hpp"
#include "main/Headless.hpp"
#include "platform/Paths.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QTimer>

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

void setupLogging(const lumora::app::AppOptions& options) {
    using namespace lumora;
    Logger& logger = Logger::instance();
    if (options.logLevel) logger.setMinLevel(*options.logLevel);
    logger.addSink(std::make_shared<ConsoleLogSink>());
    logger.addSink(std::make_shared<FileLogSink>((paths::userLogDir() / "lumora.log").string()));
}

int runGui(int argc, char** argv, lumora::app::AppOptions options) {
    using namespace lumora::app;
    QApplication qtApp(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Lumora"));
    QApplication::setOrganizationName(QStringLiteral("Lumora"));
    Preferences::load().applyTo(options);  // needs the application/organisation names set
    QApplication::setApplicationVersion(QString::fromLatin1(lumora::version::kString));
    QApplication::setWindowIcon(theme::icon(QStringLiteral("logo")));
    theme::apply(qtApp);

    AppContext context(options);
    Application application(context);
    switch (options.mode) {
        case AppMode::Hub:
            application.showHub();
            break;
        case AppMode::Editor:
            if (application.openEditor(QString::fromStdString(options.projectPath)) && options.hostPort) {
                const auto prefs = lumora::collab::CollabPreferences::load();
                const QString name = options.userName.empty() ? prefs.name : QString::fromStdString(options.userName);
                if (auto hosted = application.editor()->startHosting(
                        {*options.hostPort, name, QString::fromStdString(options.sessionPassword), prefs.publicAddress});
                    !hosted) {
                    lumora::log::error("Collab", "cannot host: {}", hosted.error().message);
                }
            }
            break;
        case AppMode::Join: {
            const auto prefs = lumora::collab::CollabPreferences::load();
            const QString name = options.userName.empty() ? prefs.name : QString::fromStdString(options.userName);
            QTimer::singleShot(0, &application, [&application, &options, name] {
                application.joinSessionAt(QString::fromStdString(options.joinAddress), name,
                                          QString::fromStdString(options.sessionPassword));
            });
            break;
        }
        case AppMode::Play:
            if (!application.runPlayer(QString::fromStdString(options.projectPath))) return 1;
            break;
        case AppMode::Headless:
            break;
    }
    return QApplication::exec();
}

} // namespace

int main(int argc, char** argv) {
    using namespace lumora;
    std::vector<std::string> args(argv + 1, argv + argc);
    auto options = app::parseCommandLine(args);
    if (!options) {
        std::fprintf(stderr, "error: %s\n\n%s", options.error().message.c_str(), app::usageText().c_str());
        return 2;
    }
    if (options->showHelp) {
        std::printf("%s", app::usageText().c_str());
        return 0;
    }
    if (options->showVersion) {
        std::printf("%s %s (%s)\n", version::kEngineName, version::kString, version::kBuildType);
        return 0;
    }
    setupLogging(*options);
    log::info("App", "{} {} ({})", version::kEngineName, version::kString, version::kBuildType);

    try {
        if (options->mode == app::AppMode::Headless) {
            // QCoreApplication: image I/O and paths without any windowing system.
            QCoreApplication core(argc, argv);
            QCoreApplication::setApplicationName(QStringLiteral("Lumora"));
            QCoreApplication::setOrganizationName(QStringLiteral("Lumora"));
            app::Preferences::load().applyTo(*options);
            return app::runHeadless(*options);
        }
        return runGui(argc, argv, *options);
    } catch (const std::exception& e) {
        log::fatal("App", "unhandled exception: {}", e.what());
        Logger::instance().flush();
        return 1;
    }
}
