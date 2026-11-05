#include "platform/Paths.hpp"

#include <cstdlib>
#include <system_error>

#if defined(LUMORA_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lumora::paths {

namespace {

std::filesystem::path envPath(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        return std::filesystem::path(value);
    }
    return {};
}

std::filesystem::path homeDir() {
#if defined(LUMORA_PLATFORM_WINDOWS)
    if (auto p = envPath("USERPROFILE"); !p.empty()) return p;
#else
    if (auto p = envPath("HOME"); !p.empty()) return p;
#endif
    return std::filesystem::current_path();
}

std::filesystem::path ensure(std::filesystem::path dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

std::filesystem::path executableDir() {
#if defined(LUMORA_PLATFORM_WINDOWS)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) return std::filesystem::current_path();
        if (len < buffer.size()) { buffer.resize(len); break; }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::filesystem::current_path();
    return exe.parent_path();
#endif
}

std::filesystem::path userConfigDir() {
#if defined(LUMORA_PLATFORM_WINDOWS)
    auto base = envPath("APPDATA");
    if (base.empty()) base = homeDir();
    return ensure(base / "Lumora");
#else
    auto base = envPath("XDG_CONFIG_HOME");
    if (base.empty()) base = homeDir() / ".config";
    return ensure(base / "Lumora");
#endif
}

std::filesystem::path userCacheDir() {
#if defined(LUMORA_PLATFORM_WINDOWS)
    auto base = envPath("LOCALAPPDATA");
    if (base.empty()) base = homeDir();
    return ensure(base / "Lumora" / "Cache");
#else
    auto base = envPath("XDG_CACHE_HOME");
    if (base.empty()) base = homeDir() / ".cache";
    return ensure(base / "Lumora");
#endif
}

std::filesystem::path userLogDir() {
#if defined(LUMORA_PLATFORM_WINDOWS)
    auto base = envPath("LOCALAPPDATA");
    if (base.empty()) base = homeDir();
    return ensure(base / "Lumora" / "Logs");
#else
    auto base = envPath("XDG_STATE_HOME");
    if (base.empty()) base = homeDir() / ".local" / "state";
    return ensure(base / "Lumora" / "logs");
#endif
}

std::filesystem::path shaderDir() {
    if (auto overridden = envPath("LUMORA_SHADER_DIR"); !overridden.empty()) {
        return overridden;
    }
    return executableDir() / "shaders";
}

std::filesystem::path defaultProjectsDir() {
    auto docs = homeDir() / "Documents";
    std::error_code ec;
    if (std::filesystem::is_directory(docs, ec)) {
        return docs / "Lumora Projects";
    }
    return homeDir() / "Lumora Projects";
}

} // namespace lumora::paths
