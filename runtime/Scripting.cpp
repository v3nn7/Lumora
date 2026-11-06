#include "runtime/Scripting.hpp"

#include "core/Log.hpp"
#include "input/InputState.hpp"
#include "runtime/AnimationSystem.hpp"
#include "runtime/AudioWorld.hpp"
#include "runtime/PhysicsWorld.hpp"
#include "scene/Prefab.hpp"
#include "scene/Scene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
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

entt::entity liveEntity(void* c, script::Entity entity) {
    const entt::entity e = toEntity(entity);
    return entity != script::kNoEntity && context(c).scene->registry().valid(e) ? e : entt::null;
}

int apiGetVelocity(void* c, script::Entity entity, float* xyz) {
    PhysicsWorld* physics = context(c).physics;
    const entt::entity e = liveEntity(c, entity);
    if (physics == nullptr || e == entt::null || xyz == nullptr) return 0;
    const glm::vec3 v = physics->linearVelocity(e);
    xyz[0] = v.x;
    xyz[1] = v.y;
    xyz[2] = v.z;
    return 1;
}

int apiSetVelocity(void* c, script::Entity entity, const float* xyz) {
    PhysicsWorld* physics = context(c).physics;
    const entt::entity e = liveEntity(c, entity);
    if (physics == nullptr || e == entt::null || xyz == nullptr) return 0;
    return physics->setLinearVelocity(e, glm::vec3(xyz[0], xyz[1], xyz[2])) ? 1 : 0;
}

int apiAddForce(void* c, script::Entity entity, const float* xyz, int impulse) {
    PhysicsWorld* physics = context(c).physics;
    const entt::entity e = liveEntity(c, entity);
    if (physics == nullptr || e == entt::null || xyz == nullptr) return 0;
    return physics->addForce(e, glm::vec3(xyz[0], xyz[1], xyz[2]), impulse != 0) ? 1 : 0;
}

int apiRaycast(void* c, const float* origin, const float* direction, float maxDistance, script::RayHit* out) {
    PhysicsWorld* physics = context(c).physics;
    if (physics == nullptr || origin == nullptr || direction == nullptr || out == nullptr) return 0;
    const auto hit = physics->raycast(glm::vec3(origin[0], origin[1], origin[2]),
                                      glm::vec3(direction[0], direction[1], direction[2]), maxDistance);
    if (!hit) return 0;
    out->entity = hit->entity == entt::null ? script::kNoEntity : static_cast<script::Entity>(entt::to_integral(hit->entity));
    out->point = {hit->point.x, hit->point.y, hit->point.z};
    out->normal = {hit->normal.x, hit->normal.y, hit->normal.z};
    out->distance = hit->distance;
    return 1;
}

script::Entity apiClone(void* c, script::Entity source) {
    const entt::entity e = liveEntity(c, source);
    if (e == entt::null) return script::kNoEntity;
    const entt::entity copy = context(c).scene->duplicateEntity(e);
    context(c).scene->updateWorldTransforms();  // physics creates the clone's body from its world pose
    return copy == entt::null ? script::kNoEntity : static_cast<script::Entity>(entt::to_integral(copy));
}

void apiDestroy(void* c, script::Entity entity) {
    const entt::entity e = liveEntity(c, entity);
    if (e != entt::null) context(c).pendingDestroy.push_back(e);
}

int apiMouseDown(void* c, int button) {
    const InputState* input = context(c).input;
    if (input == nullptr || button < 0 || button >= static_cast<int>(MouseButton::Count)) return 0;
    return input->isDown(static_cast<MouseButton>(button)) ? 1 : 0;
}

void apiMouseDelta(void* c, float* xy) {
    const InputState* input = context(c).input;
    if (xy == nullptr) return;
    const glm::vec2 d = input != nullptr ? input->mouseDelta() : glm::vec2(0.0f);
    xy[0] = d.x;
    xy[1] = d.y;
}

script::Entity apiSpawn(void* c, const char* prefab, const float* xyz) {
    Context& ctx = context(c);
    if (prefab == nullptr || ctx.projectRoot.empty()) return script::kNoEntity;
    auto& cached = ctx.prefabs[prefab];
    if (!cached) {
        const std::filesystem::path relative = std::filesystem::path(prefab).lexically_normal();
        if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
            log::warn("Script", "spawn: '{}' must be a prefab inside the project", prefab);
            return script::kNoEntity;
        }
        auto loaded = loadPrefab(ctx.projectRoot / relative);
        if (!loaded) {
            log::warn("Script", "spawn: {}", loaded.error().message);
            return script::kNoEntity;
        }
        cached = std::make_shared<const nlohmann::json>(std::move(*loaded));
    }
    auto root = instantiatePrefab(*ctx.scene, *cached);
    if (!root) {
        log::warn("Script", "spawn {}: {}", prefab, root.error().message);
        return script::kNoEntity;
    }
    if (xyz != nullptr) {
        ctx.scene->registry().get<TransformComponent>(*root).position = glm::vec3(xyz[0], xyz[1], xyz[2]);
        ctx.scene->updateWorldTransforms();
    }
    return static_cast<script::Entity>(entt::to_integral(*root));
}

int apiAnimation(void* c, script::Entity entity, const char* clip, int command) {
    AnimationSystem* animation = context(c).animation;
    const entt::entity e = liveEntity(c, entity);
    if (animation == nullptr || e == entt::null) return 0;
    switch (command) {
        case 0: animation->stop(e); return 1;
        case 1: return animation->play(e, clip != nullptr ? clip : "") ? 1 : 0;
        default: return animation->isPlaying(e) ? 1 : 0;
    }
}

int apiSound(void* c, script::Entity entity, int command) {
    AudioWorld* audio = context(c).audio;
    const entt::entity e = liveEntity(c, entity);
    if (audio == nullptr || e == entt::null) return 0;
    switch (command) {
        case 0: return audio->stop(e) ? 1 : 0;
        case 1: return audio->play(e) ? 1 : 0;
        default: return audio->isPlaying(e) ? 1 : 0;
    }
}

/// "speed=4, jump = 6.5; name=2" -> the number for `key`, or `fallback`.
double apiParameter(void* c, script::Entity entity, const char* key, double fallback) {
    const entt::entity e = liveEntity(c, entity);
    if (e == entt::null || key == nullptr) return fallback;
    const auto* script = context(c).scene->registry().try_get<ScriptComponent>(e);
    if (script == nullptr) return fallback;
    const std::string_view wanted(key);
    std::string_view rest(script->parameters);
    auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
        return v;
    };
    while (!rest.empty()) {
        const std::size_t end = rest.find_first_of(",;");
        const std::string_view item = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos || trim(item.substr(0, eq)) != wanted) continue;
        const std::string_view number = trim(item.substr(eq + 1));
        double value = fallback;
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec == std::errc() && ptr == number.data() + number.size()) return value;
        return fallback;
    }
    return fallback;
}

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
    module->collision = reinterpret_cast<CollisionFn>(symbol(module->handle_, "lumora_script_collision"));
    module->lastError = reinterpret_cast<ErrorFn>(symbol(module->handle_, "lumora_script_last_error"));
    if (abi == nullptr || count == nullptr || name == nullptr || module->create == nullptr || module->start == nullptr ||
        module->update == nullptr || module->destroy == nullptr || module->lastError == nullptr ||
        module->collision == nullptr) {
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
    api_.getVelocity = apiGetVelocity;
    api_.setVelocity = apiSetVelocity;
    api_.addForce = apiAddForce;
    api_.raycast = apiRaycast;
    api_.clone = apiClone;
    api_.destroy = apiDestroy;
    api_.mouseDown = apiMouseDown;
    api_.mouseDelta = apiMouseDelta;
    api_.parameter = apiParameter;
    api_.sound = apiSound;
    api_.spawn = apiSpawn;
    api_.animation = apiAnimation;
}

ScriptRunner::~ScriptRunner() { destroyInstances(); }

void ScriptRunner::reportFailure(const Instance& instance, const char* phase) {
    const char* message = module_->lastError();
    log::error("Script", "{}.{} on '{}' threw: {} - behaviour disabled", instance.behaviour, phase,
               apiName(&context_, instance.entity) != nullptr ? context_.nameScratch : std::string("?"),
               message != nullptr ? message : "?");
}

bool ScriptRunner::createInstance(entt::entity e, const ScriptComponent& script) {
    const auto id = static_cast<script::Entity>(entt::to_integral(e));
    scripted_.insert(id);
    if (script.behaviour.empty() || !module_) return false;
    const int index = module_->find(script.behaviour);
    if (index < 0) {
        log::warn("Script", "no behaviour named '{}' in the game library (entity '{}')", script.behaviour,
                  apiName(&context_, id) != nullptr ? context_.nameScratch : std::string("?"));
        return false;
    }
    void* object = module_->create(static_cast<std::uint32_t>(index), &api_, id);
    if (object == nullptr) {
        log::error("Script", "creating {} failed: {}", script.behaviour, module_->lastError());
        return false;
    }
    instances_.push_back({id, script.behaviour, object});
    return true;
}

void ScriptRunner::createInstances() {
    created_ = true;
    scripted_.clear();
    if (!module_) return;
    const std::size_t first = instances_.size();
    for (auto [e, script] : scene_.registry().view<ScriptComponent>().each()) createInstance(e, script);
    // start() after every instance exists, so behaviours can find each other's entities.
    for (std::size_t i = first; i < instances_.size();) {
        if (module_->start(instances_[i].object) != 0) {
            reportFailure(instances_[i], "start");
            module_->destroy(instances_[i].object);
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ScriptRunner::startNewInstances() {
    if (!module_) return;
    std::vector<entt::entity> fresh;
    for (auto [e, script] : scene_.registry().view<ScriptComponent>().each()) {
        if (!scripted_.contains(static_cast<std::uint64_t>(entt::to_integral(e)))) fresh.push_back(e);
    }
    const std::size_t first = instances_.size();
    for (entt::entity e : fresh) createInstance(e, scene_.registry().get<ScriptComponent>(e));
    for (std::size_t i = first; i < instances_.size();) {
        if (module_->start(instances_[i].object) != 0) {
            reportFailure(instances_[i], "start");
            module_->destroy(instances_[i].object);
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ScriptRunner::finishStep() {
    // Entities destroyed by scripts go now, outside any behaviour's update.
    auto pending = std::exchange(context_.pendingDestroy, {});
    for (entt::entity e : pending) {
        if (scene_.registry().valid(e)) scene_.destroyEntity(e);
    }
    // Their instances (and those of destroyed children) stop.
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (!scene_.registry().valid(toEntity(it->entity))) {
            module_->destroy(it->object);
            scripted_.erase(it->entity);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
    startNewInstances();  // clones made this step
}

void ScriptRunner::dispatchCollisions(const std::vector<std::pair<entt::entity, entt::entity>>& pairs) {
    if (!module_ || pairs.empty() || instances_.empty()) return;
    auto notify = [&](entt::entity self, entt::entity other) {
        const auto selfId = static_cast<std::uint64_t>(entt::to_integral(self));
        const auto otherId = static_cast<script::Entity>(entt::to_integral(other));
        for (std::size_t i = 0; i < instances_.size(); ++i) {
            if (instances_[i].entity != selfId) continue;
            if (module_->collision(instances_[i].object, otherId) != 0) {
                reportFailure(instances_[i], "onCollision");
                module_->destroy(instances_[i].object);
                instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return;
        }
    };
    for (const auto& [a, b] : pairs) {
        notify(a, b);
        notify(b, a);
    }
    finishStep();  // collision handlers may destroy or clone too
}

void ScriptRunner::destroyInstances() {
    if (module_) {
        for (const Instance& instance : instances_) module_->destroy(instance.object);
    }
    instances_.clear();
    scripted_.clear();
}

void ScriptRunner::update(float dt, double simulatedSeconds) {
    context_.time = simulatedSeconds;
    if (!created_) createInstances();
    // Index loop: behaviours may clone entities (appending instances only in finishStep).
    for (std::size_t i = 0; i < instances_.size();) {
        Instance& instance = instances_[i];
        if (!scene_.registry().valid(toEntity(instance.entity))) {
            ++i;  // removed by finishStep
            continue;
        }
        if (module_->update(instance.object, dt) != 0) {
            reportFailure(instance, "update");
            module_->destroy(instance.object);
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    finishStep();
}

void ScriptRunner::reload(std::shared_ptr<ScriptModule> module) {
    destroyInstances();  // with the old library still loaded: its code runs the destructors
    module_ = std::move(module);
    created_ = false;
    createInstances();
    log::info("Script", "reloaded: {} behaviour instances", instances_.size());
}

} // namespace lumora
