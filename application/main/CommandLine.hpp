#pragma once

#include "core/Error.hpp"
#include "core/Log.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lumora::app {

enum class AppMode { Hub, Editor, Play, Headless, Join };

/// Parsed command line. Modes:
///   Lumora                               Hub
///   Lumora --editor <project>            Editor
///   Lumora --project <project> --play    Standalone runtime
///   Lumora --headless [...]              Automation (no window)
///   Lumora --join <host:port>            Join a live editing session
struct AppOptions {
    AppMode mode = AppMode::Hub;
    std::string projectPath;
    bool showHelp = false;
    bool showVersion = false;

    std::optional<bool> validation;     // --validation / --no-validation (default: on in Debug)
    std::optional<std::uint32_t> gpuIndex;
    std::optional<bool> vsync;          // --no-vsync (default: preferences, then on)
    std::optional<LogLevel> logLevel;

    // Live collaboration
    std::string joinAddress;             // --join <host:port> (LAN or tunnel address)
    std::optional<std::uint16_t> hostPort;  // --host <port>: host the opened project
    std::string userName;                // --user <name>
    std::string sessionPassword;         // --password <pw>

    // Headless actions
    bool printInfo = false;
    std::string createDirectory;
    std::string createName;
    std::string templateKey = "basic";
    std::string renderOutput;
    std::uint32_t renderWidth = 1280;
    std::uint32_t renderHeight = 720;
    bool validateProject = false;
    double simulateSeconds = 0.0;
    bool buildScripts = false;  // compile the project's Scripts/ (headless)
    std::vector<std::string> importFiles;  // --import <model> (repeatable): copy + add to default scene
};

[[nodiscard]] Result<AppOptions> parseCommandLine(const std::vector<std::string>& args);
[[nodiscard]] std::string usageText();

} // namespace lumora::app
