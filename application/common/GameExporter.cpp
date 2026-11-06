#include "common/GameExporter.hpp"

#include "common/ProjectUi.hpp"
#include "core/Log.hpp"

#include <QCoreApplication>

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

} // namespace

Result<fs::path> exportGame(const GameExport& request) {
    std::error_code ec;
    const fs::path runtime = request.runtimeRoot.empty() ? toPath(QCoreApplication::applicationDirPath()).parent_path()
                                                         : request.runtimeRoot;
#if defined(_WIN32)
    const fs::path player = runtime / "bin" / "lumora-editor.exe";
#else
    const fs::path player = runtime / "bin" / "lumora-editor";
#endif
    if (!fs::is_regular_file(player, ec)) return makeError("no editor runtime at {}", runtime.string());
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
    auto copied = copyTree(request.projectRoot, request.output / "Game", [](const fs::path& relative) {
        const std::string first = relative.begin()->string();
        return first == "Saved" || first == ".git" || first == "Scripts";
    });
    if (!copied) return std::unexpected(copied.error());
    const fs::path library = request.projectRoot / "Saved" / "ScriptBuild" / "bin";
    if (fs::is_directory(library, ec)) {
        if (auto lib = copyTree(library, request.output / "Game" / "Saved" / "ScriptBuild" / "bin", {}); !lib) {
            return std::unexpected(lib.error());
        }
    }

#if defined(_WIN32)
    const fs::path launcher = request.output / (request.name + ".bat");
    std::ofstream out(launcher);
    out << "@echo off\r\nstart \"\" \"%~dp0bin\\lumora-editor.exe\" --project \"%~dp0Game\" --play %*\r\n";
#else
    const fs::path launcher = request.output / "run.sh";
    {
        std::ofstream out(launcher);
        out << "#!/bin/sh\n# Starts " << request.name << " (made with the Lumora engine).\n"
            << "HERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\n"
            << "exec \"$HERE/bin/lumora-editor\" --project \"$HERE/Game\" --play \"$@\"\n";
    }
    fs::permissions(launcher, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif
    if (!fs::exists(launcher, ec)) return makeError("cannot write {}", launcher.string());
    log::info("Export", "exported '{}' to {}", request.name, request.output.string());
    return launcher;
}

} // namespace lumora::app
