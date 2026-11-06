#include "main/CommandLine.hpp"

#include <charconv>

namespace lumora::app {

namespace {

template <class T>
Result<T> parseNumber(const std::string& flag, const std::string& text) {
    T value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return makeError("{} expects a number, got '{}'", flag, text);
    }
    return value;
}

std::optional<LogLevel> parseLevel(const std::string& s) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "info") return LogLevel::Info;
    if (s == "warn") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return std::nullopt;
}

} // namespace

Result<AppOptions> parseCommandLine(const std::vector<std::string>& args) {
    AppOptions o;
    bool editor = false, play = false, headless = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> Result<std::string> {
            if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
                return makeError("{} requires a value", a);
            }
            return args[++i];
        };
#define LUMORA_TRY_VALUE(var)                                   \
    auto var = value();                                         \
    if (!var) return std::unexpected(var.error());

        if (a == "--help" || a == "-h") {
            o.showHelp = true;
        } else if (a == "--version") {
            o.showVersion = true;
        } else if (a == "--editor") {
            LUMORA_TRY_VALUE(v);
            editor = true;
            o.projectPath = *v;
        } else if (a == "--project") {
            LUMORA_TRY_VALUE(v);
            o.projectPath = *v;
        } else if (a == "--play") {
            play = true;
        } else if (a == "--headless") {
            headless = true;
        } else if (a == "--validation") {
            o.validation = true;
        } else if (a == "--no-validation") {
            o.validation = false;
        } else if (a == "--gpu") {
            LUMORA_TRY_VALUE(v);
            auto n = parseNumber<std::uint32_t>(a, *v);
            if (!n) return std::unexpected(n.error());
            o.gpuIndex = *n;
        } else if (a == "--no-vsync") {
            o.vsync = false;
        } else if (a == "--log-level") {
            LUMORA_TRY_VALUE(v);
            o.logLevel = parseLevel(*v);
            if (!o.logLevel) return makeError("unknown log level '{}' (trace|debug|info|warn|error)", *v);
        } else if (a == "--info") {
            o.printInfo = true;
        } else if (a == "--create") {
            LUMORA_TRY_VALUE(v);
            o.createDirectory = *v;
        } else if (a == "--name") {
            LUMORA_TRY_VALUE(v);
            o.createName = *v;
        } else if (a == "--template") {
            LUMORA_TRY_VALUE(v);
            o.templateKey = *v;
        } else if (a == "--render") {
            LUMORA_TRY_VALUE(v);
            o.renderOutput = *v;
        } else if (a == "--width" || a == "--height") {
            LUMORA_TRY_VALUE(v);
            auto n = parseNumber<std::uint32_t>(a, *v);
            if (!n) return std::unexpected(n.error());
            (a == "--width" ? o.renderWidth : o.renderHeight) = *n;
        } else if (a == "--import") {
            LUMORA_TRY_VALUE(v);
            o.importFiles.push_back(*v);
        } else if (a == "--join") {
            LUMORA_TRY_VALUE(v);
            o.joinAddress = *v;
        } else if (a == "--host") {
            LUMORA_TRY_VALUE(v);
            auto n = parseNumber<std::uint32_t>(a, *v);
            if (!n) return std::unexpected(n.error());
            if (*n == 0 || *n > 65535) return makeError("--host needs a port between 1 and 65535");
            o.hostPort = static_cast<std::uint16_t>(*n);
        } else if (a == "--user") {
            LUMORA_TRY_VALUE(v);
            o.userName = *v;
        } else if (a == "--password") {
            LUMORA_TRY_VALUE(v);
            o.sessionPassword = *v;
        } else if (a == "--validate") {
            o.validateProject = true;
        } else if (a == "--build-scripts") {
            o.buildScripts = true;
        } else if (a == "--simulate") {
            LUMORA_TRY_VALUE(v);
            auto n = parseNumber<double>(a, *v);
            if (!n) return std::unexpected(n.error());
            o.simulateSeconds = *n;
        } else {
            return makeError("unknown argument '{}' (see --help)", a);
        }
#undef LUMORA_TRY_VALUE
    }

    const bool join = !o.joinAddress.empty();
    if (static_cast<int>(editor) + static_cast<int>(play) + static_cast<int>(headless) + static_cast<int>(join) > 1) {
        return makeError("--editor, --play, --headless and --join are mutually exclusive");
    }
    if (o.hostPort && (play || headless || join || o.projectPath.empty())) {
        return makeError("--host needs a project opened in the editor (--editor <project> --host <port>)");
    }
    if (join) {
        o.mode = AppMode::Join;
        return o;
    }
    if (headless) {
        o.mode = AppMode::Headless;
        const bool hasAction = o.printInfo || !o.createDirectory.empty() || !o.renderOutput.empty() ||
                               o.validateProject || o.simulateSeconds > 0.0 || !o.importFiles.empty() ||
                               o.buildScripts;
        if (!hasAction) o.printInfo = true;
        if ((!o.renderOutput.empty() || o.validateProject || o.simulateSeconds > 0.0 || !o.importFiles.empty() ||
             o.buildScripts) &&
            o.projectPath.empty()) {
            return makeError("--render/--validate/--simulate/--import/--build-scripts require --project <path>");
        }
        if (o.renderWidth == 0 || o.renderHeight == 0 || o.renderWidth > 16384 || o.renderHeight > 16384) {
            return makeError("--width/--height must be between 1 and 16384");
        }
    } else if (play) {
        if (o.projectPath.empty()) return makeError("--play requires --project <path>");
        o.mode = AppMode::Play;
    } else if (editor) {
        o.mode = AppMode::Editor;
    } else if (!o.projectPath.empty()) {
        o.mode = AppMode::Editor;  // "Lumora --project X" opens the editor
    }
    return o;
}

std::string usageText() {
    return R"(Lumora Engine

Usage:
  Lumora                                   Open Lumora Hub
  Lumora --editor <project>                Open a project in the editor
  Lumora --project <project> --play        Run a project standalone
  Lumora --headless [actions]              Automation / build mode (no window)
  Lumora --join <host:port>                Join a live session (LAN address, or the address of
                                           a playit.gg tunnel such as name.at.ply.gg:23145)
  Lumora --editor <project> --host <port>  Open a project and host a live session on <port>
                                           (TCP + UDP; tunnel it with playit.gg for the internet)

Headless actions:
  --info                                   Print engine and GPU information (default)
  --create <dir> [--name N] [--template T] Create a project (templates: empty, basic, showcase)
  --project <p> --import <model.glb>       Copy a glTF model into Content/Models and add it to
                                           the default scene (repeatable; saves the scene)
  --project <p> --validate                 Load the project and its default scene
  --project <p> --render <out.png>         Render the default scene offscreen
        [--width W] [--height H]
  --project <p> --build-scripts            Compile the project's Scripts/*.cpp into its game library
  --project <p> --simulate <seconds>       Run the fixed-step simulation (with the built game
                                           library's behaviours) and save nothing

Options:
  --gpu <index>                            Select the physical device
  --validation | --no-validation           Toggle Vulkan validation layers (default: on in Debug)
  --no-vsync                               Prefer MAILBOX/IMMEDIATE presentation
  --log-level <trace|debug|info|warn|error>
  --user <name>, --password <pw>           Name and session password for --join / --host
  --version, --help
)";
}

} // namespace lumora::app
