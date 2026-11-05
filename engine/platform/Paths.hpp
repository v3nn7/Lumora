#pragma once

#include <filesystem>

namespace lumora::paths {

/// Directory containing the running executable (shaders/ are deployed next to it).
[[nodiscard]] std::filesystem::path executableDir();
/// Per-user configuration (XDG_CONFIG_HOME/Lumora or %APPDATA%/Lumora). Created on demand.
[[nodiscard]] std::filesystem::path userConfigDir();
/// Per-user cache (XDG_CACHE_HOME/Lumora or %LOCALAPPDATA%/Lumora/Cache). Created on demand.
[[nodiscard]] std::filesystem::path userCacheDir();
/// Per-user logs/state (XDG_STATE_HOME/Lumora or %LOCALAPPDATA%/Lumora/Logs). Created on demand.
[[nodiscard]] std::filesystem::path userLogDir();
/// Directory containing compiled SPIR-V. Overridable with LUMORA_SHADER_DIR.
[[nodiscard]] std::filesystem::path shaderDir();
/// User's home/documents directory used as default project location.
[[nodiscard]] std::filesystem::path defaultProjectsDir();

} // namespace lumora::paths
