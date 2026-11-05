#include "assets/AssetLibrary.hpp"

#include "core/Log.hpp"

#include <charconv>
#include <format>

namespace lumora {

AssetLibrary::AssetLibrary(std::filesystem::path root) : root_(std::move(root)) {}

void AssetLibrary::setRoot(std::filesystem::path root) {
    std::scoped_lock lock(mutex_);
    root_ = std::move(root);
}

std::string AssetLibrary::meshKey(std::string_view relativeFile, int meshIndex) {
    return std::format("{}#{}", relativeFile, meshIndex);
}

Result<AssetLibrary::MeshRef> AssetLibrary::resolveMeshKey(std::string_view key) const {
    const auto hash = key.rfind('#');
    if (hash == std::string_view::npos || hash == 0 || hash + 1 >= key.size()) {
        return makeError("invalid mesh reference '{}' (expected <file>#<index>)", key);
    }
    int index = 0;
    const std::string_view number = key.substr(hash + 1);
    const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
    if (ec != std::errc{} || ptr != number.data() + number.size() || index < 0) {
        return makeError("invalid mesh index in '{}'", key);
    }
    const std::filesystem::path relative = std::filesystem::path(std::string(key.substr(0, hash))).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        return makeError("mesh reference '{}' must be relative to the project", key);
    }
    if (root_.empty()) {
        return makeError("no project root set to resolve '{}'", key);
    }
    return MeshRef{(root_ / relative).lexically_normal(), index};
}

Result<std::string> AssetLibrary::canonicalKey(std::string_view key) const {
    auto ref = resolveMeshKey(key);
    if (!ref) return std::unexpected(ref.error());
    return std::format("{}#{}", ref->file.generic_string(), ref->index);
}

Result<const ImportedModel*> AssetLibrary::model(const std::filesystem::path& absoluteFile) {
    const std::string id = absoluteFile.lexically_normal().generic_string();
    std::scoped_lock lock(mutex_);
    if (auto it = models_.find(id); it != models_.end()) return it->second.get();
    if (auto it = failures_.find(id); it != failures_.end()) return std::unexpected(Error{it->second});
    auto loaded = importGltf(absoluteFile);
    if (!loaded) {
        log::error("Assets", "{}", loaded.error().message);
        failures_.emplace(id, loaded.error().message);
        return std::unexpected(loaded.error());
    }
    for (const std::string& w : loaded->warnings) log::warn("Assets", "{}: {}", absoluteFile.filename().string(), w);
    log::info("Assets", "loaded {} ({} meshes, {} nodes)", absoluteFile.filename().string(), loaded->meshes.size(),
              loaded->nodes.size());
    auto& slot = models_[id];
    slot = std::make_unique<ImportedModel>(std::move(*loaded));
    return slot.get();
}

Result<const ImportedMesh*> AssetLibrary::mesh(std::string_view key) {
    auto ref = resolveMeshKey(key);
    if (!ref) return std::unexpected(ref.error());
    auto loaded = model(ref->file);
    if (!loaded) return std::unexpected(loaded.error());
    if (ref->index >= static_cast<int>((*loaded)->meshes.size())) {
        return makeError("'{}' has no mesh {}", ref->file.filename().string(), ref->index);
    }
    return &(*loaded)->meshes[static_cast<std::size_t>(ref->index)];
}

void AssetLibrary::clear() {
    std::scoped_lock lock(mutex_);
    models_.clear();
    failures_.clear();
}

} // namespace lumora
