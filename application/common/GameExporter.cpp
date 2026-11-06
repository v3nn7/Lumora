#include "common/GameExporter.hpp"

#include "common/ProjectUi.hpp"
#include "core/Log.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

namespace lumora::app {

namespace {

namespace fs = std::filesystem;

Result<> copyTree(const fs::path& from, const fs::path& to, const std::function<bool(const fs::path&)>& skip) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return makeError("cannot create {}: {}", to.string(), ec.message());
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path relative = fs::relative(it->path(), from, ec);
        if (skip && skip(relative)) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        const fs::path target = to / relative;
        if (it->is_symlink()) {
            fs::copy_symlink(it->path(), target, ec);
        } else if (it->is_directory()) {
            fs::create_directories(target, ec);
        } else {
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) return makeError("cannot copy {}: {}", it->path().string(), ec.message());
    }
    if (ec) return makeError("cannot read {}: {}", from.string(), ec.message());
    return {};
}

bool isWindows(std::string_view platform) { return platform.starts_with("windows"); }

fs::path ownInstallation() { return toPath(QCoreApplication::applicationDirPath()).parent_path(); }

} // namespace

std::string hostPlatform() {
#if defined(_WIN32)
    std::string os = "windows";
#elif defined(__APPLE__)
    std::string os = "macos";
#else
    std::string os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    return os + "-arm64";
#else
    return os + "-x64";
#endif
}

fs::path playerExecutable(std::string_view platform) {
    return fs::path("bin") / (isWindows(platform) ? "lumora-editor.exe" : "lumora-editor");
}

std::vector<ExportTarget> exportTargets(const fs::path& editorRoot) {
    const fs::path root = editorRoot.empty() ? ownInstallation() : editorRoot;
    std::vector<ExportTarget> targets{{hostPlatform(), root}};
    std::error_code ec;
    for (auto it = fs::directory_iterator(root / "platforms", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string platform = it->path().filename().string();
        if (platform == hostPlatform() || !fs::is_regular_file(it->path() / playerExecutable(platform), ec)) continue;
        targets.push_back({platform, it->path()});
    }
    std::sort(targets.begin() + 1, targets.end(), [](const ExportTarget& a, const ExportTarget& b) { return a.platform < b.platform; });
    return targets;
}

Result<fs::path> exportGame(const GameExport& request) {
    std::error_code ec;
    const std::string platform = request.platform.empty() ? hostPlatform() : request.platform;
    const bool cross = platform != hostPlatform();
    fs::path runtime = request.runtimeRoot;
    if (runtime.empty()) {
        for (const ExportTarget& t : exportTargets()) {
            if (t.platform == platform) runtime = t.runtimeRoot;
        }
        if (runtime.empty()) {
            return makeError("this editor cannot export for {}: add its build support to the editor in the Lumora Hub "
                             "(Editors)",
                             platform);
        }
    }
    const fs::path player = runtime / playerExecutable(platform);
    if (!fs::is_regular_file(player, ec)) return makeError("no {} runtime at {}", platform, runtime.string());
    auto warn = [&](std::string message) {
        log::warn("Export", "{}", message);
        if (request.warnings != nullptr) request.warnings->push_back(std::move(message));
    };
    if (fs::exists(request.output, ec) && !fs::is_empty(request.output, ec)) {
        return makeError("{} is not empty: choose an empty or new folder", request.output.string());
    }
    if (fs::weakly_canonical(request.output, ec).string().starts_with(fs::weakly_canonical(request.projectRoot, ec).string())) {
        return makeError("export into a folder outside the project");
    }

    // Runtime: everything the player needs; not the scripting SDK headers.
    for (const char* dir : {"bin", "lib", "plugins"}) {
        if (!fs::is_directory(runtime / dir, ec)) continue;
        if (auto copied = copyTree(runtime / dir, request.output / dir, {}); !copied) return std::unexpected(copied.error());
    }
    for (const char* file : {"LICENSE", "THIRD_PARTY.md", "LICENSE-DejaVu.txt"}) {
        if (fs::is_regular_file(runtime / file, ec)) fs::copy_file(runtime / file, request.output / file, ec);
    }

    // The project, without editor state: Saved/ holds caches, thumbnails and the script build
    // tree; only the built game library goes along.
    // C++ sources stay behind (the built library goes along); Lua scripts run from source.
    auto copied = copyTree(request.projectRoot, request.output / "Game", [&](const fs::path& relative) {
        const std::string first = relative.begin()->string();
        if (first == "Saved" || first == ".git") return true;
        if (first != "Scripts") return false;
        std::error_code e;
        if (fs::is_directory(request.projectRoot / relative, e)) return false;
        return relative.extension() != ".lua";
    });
    if (!copied) return std::unexpected(copied.error());
    const fs::path library = request.projectRoot / "Saved" / "ScriptBuild" / "bin";
    bool hasCpp = false;
    for (auto it = fs::recursive_directory_iterator(request.projectRoot / "Scripts", ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto ext = it->path().extension();
        hasCpp |= ext == ".cpp" || ext == ".cc" || ext == ".cxx";
    }
    ec.clear();
    if (cross) {
        // Built for the editor's platform: it would not load on the target.
        if (hasCpp) {
            warn(std::format("C++ behaviours are not included: they are compiled for {}, not {}. Lua and visual "
                             "scripts work; to ship C++ behaviours, export from an editor on {}.",
                             hostPlatform(), platform, platform));
        }
    } else if (fs::is_directory(library, ec)) {
        if (auto lib = copyTree(library, request.output / "Game" / "Saved" / "ScriptBuild" / "bin", {}); !lib) {
            return std::unexpected(lib.error());
        }
    }

    fs::path launcher;
    if (isWindows(platform)) {
        launcher = request.output / (request.name + ".bat");
        std::ofstream out(launcher, std::ios::binary);
        out << "@echo off\r\nstart \"\" \"%~dp0bin\\lumora-editor.exe\" --project \"%~dp0Game\" --play %*\r\n";
    } else {
        launcher = request.output / "run.sh";
        {
            std::ofstream out(launcher, std::ios::binary);
            // Copied from Windows the files lose their executable bit: `sh run.sh` restores it.
            out << "#!/bin/sh\n# Starts " << request.name << " (made with the Lumora engine).\n"
                << "HERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\n"
                << "[ -x \"$HERE/bin/lumora-editor\" ] || chmod +x \"$HERE/bin/lumora-editor\" \"$0\" 2>/dev/null\n"
                << "exec \"$HERE/bin/lumora-editor\" --project \"$HERE/Game\" --play \"$@\"\n";
        }
        fs::permissions(launcher, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
        ec.clear();
    }
    if (!fs::exists(launcher, ec)) return makeError("cannot write {}", launcher.string());
    log::info("Export", "exported '{}' for {} to {}", request.name, platform, request.output.string());
    return launcher;
}

} // namespace lumora::app
