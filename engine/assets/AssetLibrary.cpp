#include "assets/AssetLibrary.hpp"

#include "assets/AssetImport.hpp"

#include "core/JobSystem.hpp"
#include "core/Log.hpp"
#include "core/Timer.hpp"

#include <charconv>
#include <format>

namespace lumora {

AssetLibrary::AssetLibrary(std::filesystem::path root) : root_(std::move(root)) {}

AssetLibrary::~AssetLibrary() { waitIdle(); }

void AssetLibrary::setRoot(std::filesystem::path root) {
    std::scoped_lock lock(mutex_);
    root_ = std::move(root);
}

std::filesystem::path AssetLibrary::root() const {
    std::scoped_lock lock(mutex_);
    return root_;
}

std::string AssetLibrary::meshKey(std::string_view relativeFile, int meshIndex) {
    return std::format("{}#{}", relativeFile, meshIndex);
}

std::string AssetLibrary::textureKey(std::string_view relativeFile, int imageIndex) {
    return std::format("{}#tex{}", relativeFile, imageIndex);
}

// ---- Key resolution -----------------------------------------------------------------------

Result<std::filesystem::path> AssetLibrary::resolveRelative(std::string_view relativeText, std::string_view key) const {
    const std::filesystem::path relative = std::filesystem::path(std::string(relativeText)).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        return makeError("asset reference '{}' must be relative to the project", key);
    }
    std::scoped_lock lock(mutex_);
    if (root_.empty()) return makeError("no project root set to resolve '{}'", key);
    return (root_ / relative).lexically_normal();
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
    auto file = resolveRelative(key.substr(0, hash), key);
    if (!file) return std::unexpected(file.error());
    return MeshRef{*file, index};
}

Result<AssetLibrary::TextureRef> AssetLibrary::resolveTextureKey(std::string_view key) const {
    const auto hash = key.rfind('#');
    if (hash == std::string_view::npos) {
        auto path = resolveRelative(key, key);
        if (!path) return std::unexpected(path.error());
        if (!isImageFile(*path)) return makeError("'{}' is not a supported image (png, jpg, tga, bmp, webp, ktx2)", key);
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

Result<std::string> AssetLibrary::canonicalKey(std::string_view key) const {
    auto ref = resolveMeshKey(key);
    if (!ref) return std::unexpected(ref.error());
    return std::format("{}#{}", ref->file.generic_string(), ref->index);
}

Result<std::string> AssetLibrary::canonicalTextureKey(std::string_view key) const {
    auto ref = resolveTextureKey(key);
    if (!ref) return std::unexpected(ref.error());
    return ref->image >= 0 ? std::format("{}#tex{}", ref->file.generic_string(), ref->image) : ref->file.generic_string();
}

std::size_t AssetLibrary::invalidateFile(const std::filesystem::path& absoluteFile) {
    waitIdle();
    const std::string file = absoluteFile.lexically_normal().generic_string();
    std::scoped_lock lock(mutex_);
    const std::size_t before = models_.size() + textures_.size();
    auto fromFile = [&](const std::string& key) {
        return key == file || (key.size() > file.size() && key.starts_with(file) && key[file.size()] == '#');
    };
    std::erase_if(models_, [&](const auto& entry) { return fromFile(entry.first); });
    std::erase_if(textures_, [&](const auto& entry) { return fromFile(entry.first); });
    generation_.fetch_add(1, std::memory_order_acq_rel);
    return before - models_.size() - textures_.size();
}

void AssetLibrary::forgetFailures() {
    std::scoped_lock lock(mutex_);
    auto prune = [](auto& map) {
        std::erase_if(map, [](auto& entry) {
            std::scoped_lock slotLock(entry.second->mutex);
            return entry.second->status == std::remove_reference_t<decltype(*entry.second)>::Status::Done &&
                   !entry.second->value;
        });
    };
    prune(models_);
    prune(textures_);
}

// ---- Load slots ---------------------------------------------------------------------------

template <class T>
std::shared_ptr<AssetLibrary::Slot<T>> AssetLibrary::slot(SlotMap<T>& map, const std::string& id) {
    std::scoped_lock lock(mutex_);
    auto& entry = map[id];
    if (!entry) entry = std::make_shared<Slot<T>>();
    return entry;
}

template <class T>
Result<const T*> AssetLibrary::resolve(Slot<T>& s, const std::function<Result<T>()>& load) {
    std::unique_lock lock(s.mutex);
    if (s.status == Slot<T>::Status::Idle) {
        // Nobody is loading it: do it here (caller-runs; avoids waiting on queued jobs).
        s.status = Slot<T>::Status::Running;
        lock.unlock();
        Result<T> result = load();
        lock.lock();
        if (result) {
            s.value = std::make_unique<T>(std::move(*result));
        } else {
            s.error = result.error().message;
        }
        s.status = Slot<T>::Status::Done;
        s.done.notify_all();
    } else if (s.status == Slot<T>::Status::Running) {
        s.done.wait(lock, [&] { return s.status == Slot<T>::Status::Done; });
    }
    if (s.value) return s.value.get();
    return std::unexpected(Error{s.error});
}

template <class T>
AssetRequest<T> AssetLibrary::request(const std::shared_ptr<Slot<T>>& s, std::function<Result<T>()> load) {
    {
        std::scoped_lock lock(s->mutex);
        if (s->status == Slot<T>::Status::Done) {
            return s->value ? AssetRequest<T>{AssetState::Ready, s->value.get()} : AssetRequest<T>{AssetState::Failed, nullptr};
        }
        if (s->status == Slot<T>::Status::Running) return {};
        s->status = Slot<T>::Status::Running;
    }
    {
        std::scoped_lock lock(inflightMutex_);
        ++inflight_;
    }
    JobSystem::global().submit([this, s, load = std::move(load)] {
        Result<T> result = load();
        {
            std::scoped_lock lock(s->mutex);
            if (result) {
                s->value = std::make_unique<T>(std::move(*result));
            } else {
                s->error = result.error().message;
            }
            s->status = Slot<T>::Status::Done;
        }
        s->done.notify_all();
        std::scoped_lock lock(inflightMutex_);
        --inflight_;
        inflightDone_.notify_all();
    });
    return {};
}

std::function<Result<ImportedModel>()> AssetLibrary::modelLoader(const std::filesystem::path& file) {
    return [file] {
        Stopwatch timer;
        auto loaded = importModel(file);
        if (!loaded) {
            log::error("Assets", "{}", loaded.error().message);
            return loaded;
        }
        for (const std::string& w : loaded->warnings) log::warn("Assets", "{}: {}", file.filename().string(), w);
        log::info("Assets", "loaded {} ({} meshes, {} images, {} nodes) in {:.1f} ms", file.filename().string(),
                  loaded->meshes.size(), loaded->images.size(), loaded->nodes.size(), timer.elapsedMs());
        return loaded;
    };
}

std::function<Result<ImageData>()> AssetLibrary::textureLoader(const TextureRef& ref, std::string key) {
    return [this, ref, key = std::move(key)]() -> Result<ImageData> {
        Stopwatch timer;
        Result<ImageData> decoded = makeError("unreachable");
        if (ref.image < 0) {
            decoded = loadImageFile(ref.file);
        } else {
            auto loaded = model(ref.file);  // runs inline or joins an in-progress load
            if (!loaded) {
                decoded = std::unexpected(loaded.error());
            } else if (ref.image >= static_cast<int>((*loaded)->images.size())) {
                decoded = makeError("'{}' has no image {}", ref.file.filename().string(), ref.image);
            } else {
                const ImportedImage& image = (*loaded)->images[static_cast<std::size_t>(ref.image)];
                decoded = decodeImage(image.encoded);
                if (decoded && image.cutOutBackground) cutOutPaddedBackground(*decoded);
                if (!decoded) {
                    decoded = makeError("{} image '{}': {}", ref.file.filename().string(), image.name, decoded.error().message);
                }
            }
        }
        if (!decoded) {
            log::error("Assets", "{}", decoded.error().message);
        } else {
            log::debug("Assets", "decoded texture {} ({}x{}) in {:.1f} ms", key, decoded->width, decoded->height,
                       timer.elapsedMs());
        }
        return decoded;
    };
}

// ---- Public access ------------------------------------------------------------------------

Result<const ImportedModel*> AssetLibrary::model(const std::filesystem::path& absoluteFile) {
    const std::filesystem::path file = absoluteFile.lexically_normal();
    auto s = slot(models_, file.generic_string());
    return resolve<ImportedModel>(*s, modelLoader(file));
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

Result<const ImageData*> AssetLibrary::texture(std::string_view key) {
    auto ref = resolveTextureKey(key);
    if (!ref) return std::unexpected(ref.error());
    auto id = canonicalTextureKey(key);
    auto s = slot(textures_, *id);
    return resolve<ImageData>(*s, textureLoader(*ref, std::string(key)));
}

AssetRequest<ImportedMesh> AssetLibrary::requestMesh(std::string_view key) {
    auto ref = resolveMeshKey(key);
    if (!ref) return {AssetState::Failed, nullptr};
    auto s = slot(models_, ref->file.generic_string());
    const AssetRequest<ImportedModel> m = request<ImportedModel>(s, modelLoader(ref->file));
    if (m.state != AssetState::Ready) return {m.state, nullptr};
    if (ref->index >= static_cast<int>(m.value->meshes.size())) return {AssetState::Failed, nullptr};
    return {AssetState::Ready, &m.value->meshes[static_cast<std::size_t>(ref->index)]};
}

AssetRequest<ImageData> AssetLibrary::requestTexture(std::string_view key) {
    auto ref = resolveTextureKey(key);
    if (!ref) return {AssetState::Failed, nullptr};
    auto id = canonicalTextureKey(key);
    auto s = slot(textures_, *id);
    return request<ImageData>(s, textureLoader(*ref, std::string(key)));
}

std::shared_ptr<const MaterialAsset> AssetLibrary::material(std::string_view key) {
    if (key.empty()) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(materialMutex_);
    MaterialEntry& entry = materials_[std::string(key)];
    if (entry.checked != std::chrono::steady_clock::time_point{} && now - entry.checked < std::chrono::milliseconds(500)) {
        return entry.value;
    }
    entry.checked = now;
    auto path = resolveRelative(key, key);
    if (!path) {
        if (!entry.reported) log::warn("Assets", "{}", path.error().message);
        entry.reported = true;
        entry.value = nullptr;
        return nullptr;
    }
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(*path, ec);
    if (ec) {
        if (!entry.reported) log::warn("Assets", "material {} not found", std::string(key));
        entry.reported = true;
        entry.value = nullptr;
        entry.stamp = {};
        return nullptr;
    }
    if (entry.value && stamp == entry.stamp) return entry.value;
    if (!entry.value && entry.reported && stamp == entry.stamp) return nullptr;  // still the broken file
    entry.stamp = stamp;
    auto loaded = loadMaterial(*path);
    if (!loaded) {
        log::warn("Assets", "{}", loaded.error().message);
        entry.reported = true;
        entry.value = nullptr;
        return nullptr;
    }
    entry.reported = false;
    entry.value = std::make_shared<const MaterialAsset>(std::move(*loaded));
    return entry.value;
}

void AssetLibrary::reloadMaterial(std::string_view key) {
    std::scoped_lock lock(materialMutex_);
    materials_.erase(std::string(key));
}

void AssetLibrary::waitIdle() {
    std::unique_lock lock(inflightMutex_);
    inflightDone_.wait(lock, [this] { return inflight_ == 0; });
}

void AssetLibrary::clear() {
    waitIdle();
    std::scoped_lock lock(mutex_);
    models_.clear();
    textures_.clear();
    std::scoped_lock materialLock(materialMutex_);
    materials_.clear();
}

} // namespace lumora
