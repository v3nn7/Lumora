#pragma once

#include "assets/GltfImporter.hpp"
#include "assets/Image.hpp"
#include "core/Error.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lumora {

/// Resolves project-relative asset references and caches loaded model files.
///
/// Mesh keys have the form "<project-relative file>#<mesh index>", e.g.
/// "Content/Models/Crate.glb#0". Texture keys are either an image inside a model,
/// "<file>#tex<image index>", or a standalone image file, "Content/Textures/Wood.png".
/// Keys are portable (forward slashes, relative to the project root, never absolute and never
/// escaping the project with "..").
class AssetLibrary {
public:
    explicit AssetLibrary(std::filesystem::path root = {});

    void setRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    [[nodiscard]] static std::string meshKey(std::string_view relativeFile, int meshIndex);
    [[nodiscard]] static std::string textureKey(std::string_view relativeFile, int imageIndex);

    struct MeshRef {
        std::filesystem::path file;  // absolute
        int index = 0;
    };
    [[nodiscard]] Result<MeshRef> resolveMeshKey(std::string_view key) const;
    /// Absolute-path identity for caches shared across projects (e.g. GPU meshes).
    [[nodiscard]] Result<std::string> canonicalKey(std::string_view key) const;

    /// Loads (once) and returns a mesh. Failures are cached too, so a broken reference is
    /// reported once instead of every frame. Thread-safe.
    [[nodiscard]] Result<const ImportedMesh*> mesh(std::string_view key);
    [[nodiscard]] Result<const ImportedModel*> model(const std::filesystem::path& absoluteFile);
    /// Decoded RGBA8 texture (cached; failures cached and reported once). Thread-safe.
    [[nodiscard]] Result<const ImageData*> texture(std::string_view key);
    [[nodiscard]] Result<std::string> canonicalTextureKey(std::string_view key) const;
    /// Drops cached models (e.g. after re-importing a file).
    void clear();

private:
    struct TextureRef {
        std::filesystem::path file;  // absolute
        int image = -1;              // >= 0: image inside a model file
    };
    [[nodiscard]] Result<std::filesystem::path> resolveRelative(std::string_view relative, std::string_view key) const;
    [[nodiscard]] Result<TextureRef> resolveTextureKey(std::string_view key) const;
    [[nodiscard]] Result<const ImportedModel*> modelLocked(const std::filesystem::path& absoluteFile);

    std::filesystem::path root_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<ImportedModel>> models_;
    std::unordered_map<std::string, std::string> failures_;
    std::unordered_map<std::string, std::unique_ptr<ImageData>> textures_;
};

} // namespace lumora
