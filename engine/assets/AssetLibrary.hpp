#pragma once

#include "assets/GltfImporter.hpp"
#include "assets/Image.hpp"
#include "core/Error.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lumora {

enum class AssetState { Pending, Ready, Failed };

template <class T>
struct AssetRequest {
    AssetState state = AssetState::Pending;
    const T* value = nullptr;  // set when Ready
};

/// Resolves project-relative asset references and loads them, synchronously or in the
/// background (JobSystem::global()).
///
/// Mesh keys have the form "<project-relative file>#<mesh index>", e.g.
/// "Content/Models/Crate.glb#0". Texture keys are either an image inside a model,
/// "<file>#tex<image index>", or a standalone image file, "Content/Textures/Wood.png".
/// Keys are portable (forward slashes, relative to the project root, never absolute and never
/// escaping the project with "..").
///
/// Every asset is loaded at most once: concurrent synchronous and asynchronous requests share
/// one load, and failures are cached (reported once). Thread-safe.
class AssetLibrary {
public:
    explicit AssetLibrary(std::filesystem::path root = {});
    ~AssetLibrary();
    AssetLibrary(const AssetLibrary&) = delete;
    AssetLibrary& operator=(const AssetLibrary&) = delete;

    void setRoot(std::filesystem::path root);
    [[nodiscard]] std::filesystem::path root() const;

    [[nodiscard]] static std::string meshKey(std::string_view relativeFile, int meshIndex);
    [[nodiscard]] static std::string textureKey(std::string_view relativeFile, int imageIndex);

    struct MeshRef {
        std::filesystem::path file;  // absolute
        int index = 0;
    };
    [[nodiscard]] Result<MeshRef> resolveMeshKey(std::string_view key) const;
    /// Absolute-path identities for caches shared across projects (e.g. GPU resources).
    [[nodiscard]] Result<std::string> canonicalKey(std::string_view key) const;
    [[nodiscard]] Result<std::string> canonicalTextureKey(std::string_view key) const;

    // ---- Blocking access (importers, tools, offscreen captures) ----------------------------
    [[nodiscard]] Result<const ImportedModel*> model(const std::filesystem::path& absoluteFile);
    [[nodiscard]] Result<const ImportedMesh*> mesh(std::string_view key);
    [[nodiscard]] Result<const ImageData*> texture(std::string_view key);

    // ---- Non-blocking access (the renderer each frame) -------------------------------------
    [[nodiscard]] AssetRequest<ImportedMesh> requestMesh(std::string_view key);
    [[nodiscard]] AssetRequest<ImageData> requestTexture(std::string_view key);

    /// Blocks until every background load started so far has finished.
    void waitIdle();
    /// Drops all cached assets (waits for background loads first).
    void clear();
    /// Forgets cached failures so the next request tries again (e.g. a missing file arrived).
    void forgetFailures();

private:
    template <class T>
    struct Slot {
        std::mutex mutex;
        std::condition_variable done;
        enum class Status { Idle, Running, Done } status = Status::Idle;
        std::unique_ptr<T> value;
        std::string error;
    };
    template <class T>
    using SlotMap = std::unordered_map<std::string, std::shared_ptr<Slot<T>>>;

    struct TextureRef {
        std::filesystem::path file;  // absolute
        int image = -1;              // >= 0: image inside a model file
    };

    template <class T>
    std::shared_ptr<Slot<T>> slot(SlotMap<T>& map, const std::string& id);
    template <class T>
    Result<const T*> resolve(Slot<T>& slot, const std::function<Result<T>()>& load);
    template <class T>
    AssetRequest<T> request(const std::shared_ptr<Slot<T>>& slot, std::function<Result<T>()> load);

    [[nodiscard]] Result<std::filesystem::path> resolveRelative(std::string_view relative, std::string_view key) const;
    [[nodiscard]] Result<TextureRef> resolveTextureKey(std::string_view key) const;
    [[nodiscard]] std::function<Result<ImportedModel>()> modelLoader(const std::filesystem::path& file);
    [[nodiscard]] std::function<Result<ImageData>()> textureLoader(const TextureRef& ref, std::string key);

    mutable std::mutex mutex_;  // guards root_ and the slot maps (never held while loading)
    std::filesystem::path root_;
    SlotMap<ImportedModel> models_;
    SlotMap<ImageData> textures_;

    std::mutex inflightMutex_;
    std::condition_variable inflightDone_;
    int inflight_ = 0;
};

} // namespace lumora
