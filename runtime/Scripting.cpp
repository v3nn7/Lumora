#include "runtime/Scripting.hpp"

#include "core/Log.hpp"
#include "input/InputState.hpp"
#include "scene/Scene.hpp"

#include <atomic>
#include <format>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace lumora {

static_assert(static_cast<int>(Key::W) == static_cast<int>(script::Key::W) &&
                  static_cast<int>(Key::Escape) == static_cast<int>(script::Key::Escape) &&
                  static_cast<int>(Key::Count) == static_cast<int>(script::Key::Escape) + 1,
              "lumora::script::Key must list the engine's keys in the same order");

namespace {

void* openLibrary(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
    HMODULE handle = LoadLibraryW(path.wstring().c_str());
    if (handle == nullptr) error = std::system_category().message(static_cast<int>(GetLastError()));
    return reinterpret_cast<void*>(handle);
#else
    // RTLD_LOCAL: two generations of the library never resolve each other's symbols.
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* message = dlerror();
        error = message != nullptr ? message : "dlopen failed";
    }
    return handle;
#endif
}

void* symbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void closeLibrary(void* handle) {
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

unsigned long processId() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

entt::entity toEntity(script::Entity e) {
    return static_cast<entt::entity>(static_cast<std::underlying_type_t<entt::entity>>(e));
}

using Context = ScriptRunner::Context;
Context& context(void* c) { return *static_cast<Context*>(c); }

// ---- EngineApi callbacks (C ABI; never throw across the library boundary) ------------------

void apiLog(void* c, int level, const char* message) {
    (void)c;
    const std::string_view text = message != nullptr ? message : "";
    if (level >= 2) {
        log::error("Script", "{}", text);
    } else if (level == 1) {
        log::warn("Script", "{}", text);
    } else {
        log::info("Script", "{}", text);
    }
}

script::Entity apiFind(void* c, const char* name) {
    if (name == nullptr) return script::kNoEntity;
    for (auto [e, n] : context(c).scene->registry().view<NameComponent>().each()) {
        if (n.name == name) return static_cast<script::Entity>(entt::to_integral(e));
    }
    return script::kNoEntity;
}

const char* apiName(void* c, script::Entity entity) {
    Context& ctx = context(c);
    const auto& reg = ctx.scene->registry();
    const entt::entity e = toEntity(entity);
    if (entity == script::kNoEntity || !reg.valid(e)) return nullptr;
    const auto* n = reg.try_get<NameComponent>(e);
    ctx.nameScratch = n != nullptr ? n->name : std::string();
    return ctx.nameScratch.c_str();
}

glm::vec3* transformField(void* c, script::Entity entity, int which) {
    auto& reg = context(c).scene->registry();
    const entt::entity e = toEntity(entity);
    if (entity == script::kNoEntity || !reg.valid(e)) return nullptr;
    auto* t = reg.try_get<TransformComponent>(e);
    if (t == nullptr) return nullptr;
    switch (which) {
        case 0: return &t->position;
        case 1: return &t->rotationDeg;
        case 2: return &t->scale;
        default: return nullptr;
    }
}

int apiGetTransform(void* c, script::Entity entity, int which, float* xyz) {
    const glm::vec3* v = transformField(c, entity, which);
    if (v == nullptr || xyz == nullptr) return 0;
    xyz[0] = v->x;
    xyz[1] = v->y;
    xyz[2] = v->z;
    return 1;
}

int apiSetTransform(void* c, script::Entity entity, int which, const float* xyz) {
    glm::vec3* v = transformField(c, entity, which);
    if (v == nullptr || xyz == nullptr) return 0;
    *v = glm::vec3(xyz[0], xyz[1], xyz[2]);
    return 1;
}

int apiKeyDown(void* c, int key) {
    const InputState* input = context(c).input;
    if (input == nullptr || key < 0 || key >= static_cast<int>(Key::Count)) return 0;
    return input->isDown(static_cast<Key>(key)) ? 1 : 0;
}

double apiTime(void* c) { return context(c).time; }

} // namespace

// ---- ScriptModule ------------------------------------------------------------------------------

Result<std::shared_ptr<ScriptModule>> ScriptModule::load(const std::filesystem::path& library,
                                                         const std::filesystem::path& loadDir) {
    static std::atomic<unsigned> generation{0};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(library, ec)) {
        return makeError("game library {} does not exist (build the scripts first)", library.string());
    }
    std::filesystem::create_directories(loadDir, ec);
    const auto copy = loadDir / std::format("{}-{}-{}{}", library.stem().string(), processId(), ++generation,
                                            library.extension().string());
    std::filesystem::copy_file(library, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return makeError("cannot copy {} to {}: {}", library.string(), copy.string(), ec.message());

    std::shared_ptr<ScriptModule> module(new ScriptModule());
    module->copy_ = copy;
    std::string error;
    module->handle_ = openLibrary(copy, error);
    if (module->handle_ == nullptr) return makeError("cannot load {}: {}", library.string(), error);

    using AbiFn = std::uint32_t (*)();
    using CountFn = std::uint32_t (*)();
    using NameFn = const char* (*)(std::uint32_t);
    auto abi = reinterpret_cast<AbiFn>(symbol(module->handle_, "lumora_script_abi"));
    auto count = reinterpret_cast<CountFn>(symbol(module->handle_, "lumora_script_count"));
    auto name = reinterpret_cast<NameFn>(symbol(module->handle_, "lumora_script_name"));
    module->create = reinterpret_cast<CreateFn>(symbol(module->handle_, "lumora_script_create"));
    module->start = reinterpret_cast<StepFn>(symbol(module->handle_, "lumora_script_start"));
    module->update = reinterpret_cast<UpdateFn>(symbol(module->handle_, "lumora_script_update"));
    module->destroy = reinterpret_cast<DestroyFn>(symbol(module->handle_, "lumora_script_destroy"));
    module->lastError = reinterpret_cast<ErrorFn>(symbol(module->handle_, "lumora_script_last_error"));
    if (abi == nullptr || count == nullptr || name == nullptr || module->create == nullptr || module->start == nullptr ||
        module->update == nullptr || module->destroy == nullptr || module->lastError == nullptr) {
        return makeError("{} is not a Lumora game library (entry points missing)", library.string());
    }
    if (abi() != LUMORA_SCRIPT_ABI) {
        return makeError("{} uses scripting ABI {}, this editor {} - rebuild the scripts", library.string(), abi(),
                         LUMORA_SCRIPT_ABI);
    }
    for (std::uint32_t i = 0, n = count(); i < n; ++i) {
        const char* behaviour = name(i);
        module->names_.emplace_back(behaviour != nullptr ? behaviour : "");
    }
    log::info("Script", "loaded {} ({} behaviours)", library.filename().string(), module->names_.size());
    return module;
}

ScriptModule::~ScriptModule() {
    if (handle_ != nullptr) closeLibrary(handle_);
    std::error_code ec;
    if (!copy_.empty()) std::filesystem::remove(copy_, ec);
}

int ScriptModule::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return static_cast<int>(i);
    }
    return -1;
}

// ---- ScriptRunner ----------------------------------------------------------------------------

ScriptRunner::ScriptRunner(Scene& scene, std::shared_ptr<ScriptModule> module, const InputState* input)
    : scene_(scene), module_(std::move(module)) {
    context_.scene = &scene_;
    context_.input = input;
    api_.abi = LUMORA_SCRIPT_ABI;
    api_.context = &context_;
    api_.log = apiLog;
    api_.find = apiFind;
    api_.name = apiName;
    api_.getTransform = apiGetTransform;
    api_.setTransform = apiSetTransform;
    api_.keyDown = apiKeyDown;
    api_.time = apiTime;
}

ScriptRunner::~ScriptRunner() { destroyInstances(); }

void ScriptRunner::reportFailure(const Instance& instance, const char* phase) {
    const char* message = module_->lastError();
    log::error("Script", "{}.{} on '{}' threw: {} - behaviour disabled", instance.behaviour, phase,
               apiName(&context_, instance.entity) != nullptr ? context_.nameScratch : std::string("?"),
               message != nullptr ? message : "?");
}

void ScriptRunner::createInstances() {
    created_ = true;
    if (!module_) return;
    for (auto [e, script] : scene_.registry().view<ScriptComponent>().each()) {
        if (script.behaviour.empty()) continue;
        const int index = module_->find(script.behaviour);
        const auto id = static_cast<script::Entity>(entt::to_integral(e));
        if (index < 0) {
            log::warn("Script", "no behaviour named '{}' in the game library (entity '{}')", script.behaviour,
                      apiName(&context_, id) != nullptr ? context_.nameScratch : std::string("?"));
            continue;
        }
        void* object = module_->create(static_cast<std::uint32_t>(index), &api_, id);
        if (object == nullptr) {
            log::error("Script", "creating {} failed: {}", script.behaviour, module_->lastError());
            continue;
        }
        instances_.push_back({id, script.behaviour, object});
    }
    // start() after every instance exists, so behaviours can find each other's entities.
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (module_->start(it->object) != 0) {
            reportFailure(*it, "start");
            module_->destroy(it->object);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptRunner::destroyInstances() {
    if (module_) {
        for (const Instance& instance : instances_) module_->destroy(instance.object);
    }
    instances_.clear();
}

void ScriptRunner::update(float dt, double simulatedSeconds) {
    context_.time = simulatedSeconds;
    if (!created_) createInstances();
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (!scene_.registry().valid(toEntity(it->entity))) {  // entity removed meanwhile
            module_->destroy(it->object);
            it = instances_.erase(it);
            continue;
        }
        if (module_->update(it->object, dt) != 0) {
            reportFailure(*it, "update");
            module_->destroy(it->object);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptRunner::reload(std::shared_ptr<ScriptModule> module) {
    destroyInstances();  // with the old library still loaded: its code runs the destructors
    module_ = std::move(module);
    created_ = false;
    createInstances();
    log::info("Script", "reloaded: {} behaviour instances", instances_.size());
}

} // namespace lumora
