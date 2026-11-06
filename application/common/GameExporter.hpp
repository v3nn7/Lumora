#pragma once

#include "core/Error.hpp"

#include <filesystem>
#include <string>

namespace lumora::app {

/// Writes a standalone copy of a game: the runtime of the editor that runs this code (its bin/,
/// lib/ and plugins/, i.e. the player, engine modules, shaders and Qt), the project (without
/// Saved/, except the built game library) and a launcher:
///
///   <out>/run.sh or <Name>.bat        starts the player full of the game
///   <out>/bin, lib, plugins           runtime
///   <out>/Game/                       the project
///
/// The output directory must be empty or not exist. Returns the launcher's path.
struct GameExport {
    std::filesystem::path projectRoot;
    std::string name;                 // the game's name (launcher file on Windows)
    std::filesystem::path output;
    /// Where the runtime comes from; empty = the running executable's installation.
    std::filesystem::path runtimeRoot;
};
[[nodiscard]] Result<std::filesystem::path> exportGame(const GameExport& request);

} // namespace lumora::app
