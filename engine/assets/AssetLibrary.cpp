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

std::string AssetLibrary::textureKey(std::string_view relativeFile, int imageIndex) {
    return std::format("{}#tex{}", relativeFile, imageIndex);
}

Result<std::filesystem::path> AssetLibrary::resolveRelative(std::string_view relativeText, std::string_view key) const {
    const std::filesystem::path relative = std::filesystem::path(std::string(relativeText)).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        return makeError("asset reference '{}' must be relative to the project", key);
    }
    if (root_.empty()) {
        return makeError("no project root set to resolve '{}'", key);
    }
    return (root_ / relative).lexically_normal();
}

Result<AssetLibrary::TextureRef> AssetLibrary::resolveTextureKey(std::string_view key) const {
    const auto hash = key.rfind('#');
    if (hash == std::string_view::npos) {
        auto path = resolveRelative(key, key);
        if (!path) return std::unexpected(path.error());
        if (!isImageFile(*path)) return makeError("'{}' is not a supported image (png, jpg, tga, bmp)", key);
        return TextureRef{*path, -1};
    }
    const std::string_view suffix = key.substr(hash + 1);
    if (!suffix.starts_with("tex")) return makeError("invalid texture reference '{}'", key);
    int index = -1;
    const std::string_view number = suffix.substr(3);
    const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
    if (number.empty() || ec != std::errc{} || ptr != number.data() + number.size() || index < 0) {
        return makeError("invalid texture index in '{}'", key);
    }
    auto path = resolveRelative(key.substr(0, hash), key);
    if (!path) return std::unexpected(path.error());
    return TextureRef{*path, index};
}

Result<std::string> AssetLibrary::canonicalTextureKey(std::string_view key) const {
    auto ref = resolveTextureKey(key);
    if (!ref) return std::unexpected(ref.error());
    return ref->image >= 0 ? std::format("{}#tex{}", ref->file.generic_string(), ref->image) : ref->file.generic_string();
}

Result<const ImageData*> AssetLibrary::texture(std::string_view key) {
    auto ref = resolveTextureKey(key);
    if (!ref) return std::unexpected(ref.error());
    const std::string id = ref->image >= 0 ? std::format("{}#tex{}", ref->file.generic_string(), ref->image)
                                           : ref->file.generic_string();
    std::scoped_lock lock(mutex_);
    if (auto it = textures_.find(id); it != textures_.end()) return it->second.get();
    if (auto it = failures_.find(id); it != failures_.end()) return std::unexpected(Error{it->second});
    Result<ImageData> decoded = makeError("unreachable");
    if (ref->image < 0) {
        decoded = loadImageFile(ref->file);
    } else {
        auto model = modelLocked(ref->file);
        if (!model) return std::unexpected(model.error());
        if (ref->image >= static_cast<int>((*model)->images.size())) {
            decoded = makeError("'{}' has no image {}", ref->file.filename().string(), ref->image);
        } else {
            const ImportedImage& image = (*model)->images[static_cast<std::size_t>(ref->image)];
            decoded = decodeImage(image.encoded);
            if (!decoded) decoded = makeError("{} image '{}': {}", ref->file.filename().string(), image.name,
                                              decoded.error().message);
        }
    }
    if (!decoded) {
        log::error("Assets", "{}", decoded.error().message);
        failures_.emplace(id, decoded.error().message);
        return std::unexpected(decoded.error());
    }
    log::debug("Assets", "decoded texture {} ({}x{})", key, decoded->width, decoded->height);
    auto& slot = textures_[id];
    slot = std::make_unique<ImageData>(std::move(*decoded));
    return slot.get();
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
    std::scoped_lock lock(mutex_);
    return modelLocked(absoluteFile);
}

Result<const ImportedModel*> AssetLibrary::modelLocked(const std::filesystem::path& absoluteFile) {
    const std::string id = absoluteFile.lexically_normal().generic_string();
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
    textures_.clear();
}

} // namespace lumora
