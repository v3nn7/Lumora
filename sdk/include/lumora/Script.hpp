// Lumora gameplay scripting API.
//
// Write behaviours in your project's Scripts/ folder:
//
//     #include <lumora/Script.hpp>
//     using namespace lumora::script;
//
//     class Bobber : public Behaviour {
//     public:
//         void start() override { base_ = position(); }
//         void update(float dt) override {
//             elapsed_ += dt;
//             setPosition(base_ + Vec3{0.0f, std::sin(elapsed_ * 2.0f) * 0.5f, 0.0f});
//         }
//     private:
//         Vec3 base_;
//         float elapsed_ = 0.0f;
//     };
//     LUMORA_BEHAVIOUR(Bobber);
//
// Behaviours can also drive physics (addForce, addImpulse, setVelocity, raycast), react to
// collisions (onCollision), clone and destroy entities, read the keyboard and mouse, and read
// numbers from the Script component's Parameters field (param("speed", 4.0f)).
//
// then add a Script component with Behaviour "Bobber" to an entity and press Play. The editor
// compiles Scripts/*.cpp into a shared library and reloads it whenever a file changes, also
// during play (behaviours are then created again and start() runs again).
//
// The engine and the game library talk through a C function table (EngineApi) and a few
// exported C functions, so the library may be built by another compiler than the editor.
// Exceptions thrown by a behaviour are caught and reported; the behaviour is then disabled.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#define LUMORA_SCRIPT_ABI 2u

namespace lumora::script {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    friend Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    friend Vec3 operator*(float s, Vec3 a) { return a * s; }
    Vec3& operator+=(Vec3 b) { return *this = *this + b; }
    [[nodiscard]] float length() const { return std::sqrt(x * x + y * y + z * z); }
};

/// Keys the simulation sees (the same set the editor viewport tracks).
enum class Key : int {
    W, A, S, D, Q, E, F,
    Up, Down, Left, Right,
    Space, LeftShift, LeftCtrl, LeftAlt, Escape,
};

using Entity = std::uint64_t;
inline constexpr Entity kNoEntity = ~Entity{0};

enum class MouseButton : int { Left, Right, Middle };

/// Result of Behaviour::raycast.
struct RayHit {
    Entity entity = ~Entity{0};
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};

/// Services of the engine, as plain C function pointers (stable for one LUMORA_SCRIPT_ABI).
struct EngineApi {
    std::uint32_t abi;
    void* context;
    void (*log)(void* context, int level, const char* message);  // level 0 info, 1 warning, 2 error
    Entity (*find)(void* context, const char* name);              // kNoEntity if none
    const char* (*name)(void* context, Entity entity);            // valid until the next call
    // which: 0 position (m), 1 rotation (Euler degrees), 2 scale. Return 0 if the entity is gone.
    int (*getTransform)(void* context, Entity entity, int which, float* xyz);
    int (*setTransform)(void* context, Entity entity, int which, const float* xyz);
    int (*keyDown)(void* context, int key);
    double (*time)(void* context);  // simulated seconds since Play
    // ABI 2: physics, entities, mouse, parameters.
    int (*getVelocity)(void* context, Entity entity, float* xyz);        // 0 if no dynamic body
    int (*setVelocity)(void* context, Entity entity, const float* xyz);
    int (*addForce)(void* context, Entity entity, const float* xyz, int impulse);
    int (*raycast)(void* context, const float* origin, const float* direction, float maxDistance, RayHit* hit);
    Entity (*clone)(void* context, Entity source);  // copy with children and components
    void (*destroy)(void* context, Entity entity);  // removed at the end of the step
    int (*mouseDown)(void* context, int button);
    void (*mouseDelta)(void* context, float* xy);  // pixels moved this frame
    double (*parameter)(void* context, Entity entity, const char* name, double fallback);
    int (*sound)(void* context, Entity entity, int command);  // 0 stop, 1 play, 2 is playing
    Entity (*spawn)(void* context, const char* prefab, const float* position);  // kNoEntity on failure
};

class Behaviour;

namespace detail {
struct Registration {
    const char* name;
    Behaviour* (*create)();
};
// Defined once per game library (lumora/ScriptModuleMain.hpp), not inline: an inline function's
// static would be a process-wide GNU_UNIQUE symbol shared by every library generation and
// would keep old libraries from unloading.
std::vector<Registration>& registry();
struct Registrar {
    Registrar(const char* name, Behaviour* (*create)()) { registry().push_back({name, create}); }
};
struct Access;
} // namespace detail

/// Base class of gameplay behaviours: one instance per entity with a Script component naming
/// it. start() runs once when play begins (or after a reload), update() every fixed step.
class Behaviour {
public:
    virtual ~Behaviour() = default;
    virtual void start() {}
    virtual void update(float dt) { (void)dt; }
    /// This entity's body started touching another one (needs Collider components).
    virtual void onCollision(Entity other) { (void)other; }

protected:
    [[nodiscard]] Entity entity() const { return entity_; }
    [[nodiscard]] std::string name() const { return name(entity_); }
    [[nodiscard]] std::string name(Entity e) const {
        const char* n = api_->name(api_->context, e);
        return n != nullptr ? n : std::string();
    }
    [[nodiscard]] Entity find(std::string_view entityName) const {
        return api_->find(api_->context, std::string(entityName).c_str());
    }

    [[nodiscard]] Vec3 position() const { return get(entity_, 0); }
    [[nodiscard]] Vec3 rotation() const { return get(entity_, 1); }
    [[nodiscard]] Vec3 scale() const { return get(entity_, 2); }
    void setPosition(Vec3 v) const { set(entity_, 0, v); }
    void setRotation(Vec3 degrees) const { set(entity_, 1, degrees); }
    void setScale(Vec3 v) const { set(entity_, 2, v); }
    [[nodiscard]] Vec3 positionOf(Entity e) const { return get(e, 0); }
    void setPositionOf(Entity e, Vec3 v) const { set(e, 0, v); }

    // Physics (dynamic Rigid Body + Collider). Forces in newtons, impulses in newton-seconds.
    [[nodiscard]] Vec3 velocity() const {
        float v[3] = {0.0f, 0.0f, 0.0f};
        api_->getVelocity(api_->context, entity_, v);
        return {v[0], v[1], v[2]};
    }
    void setVelocity(Vec3 v) const {
        const float xyz[3] = {v.x, v.y, v.z};
        api_->setVelocity(api_->context, entity_, xyz);
    }
    void addForce(Vec3 f) const { push(f, 0); }
    void addImpulse(Vec3 i) const { push(i, 1); }
    /// Closest collider along a ray; false if nothing within maxDistance.
    bool raycast(Vec3 origin, Vec3 direction, float maxDistance, RayHit& hit) const {
        const float o[3] = {origin.x, origin.y, origin.z};
        const float d[3] = {direction.x, direction.y, direction.z};
        return api_->raycast(api_->context, o, d, maxDistance, &hit) != 0;
    }

    // Entities
    /// Copies an entity (with its children and components, including scripts) into the scene.
    [[nodiscard]] Entity clone(Entity source) const { return api_->clone(api_->context, source); }
    /// Adds a copy of a prefab (project-relative .lprefab) at a position; its scripts start at
    /// the end of this step. kNoEntity if the prefab cannot be loaded.
    Entity spawn(std::string_view prefab, Vec3 position) const {
        const float xyz[3] = {position.x, position.y, position.z};
        return api_->spawn(api_->context, std::string(prefab).c_str(), xyz);
    }
    /// Removes an entity (and its children) at the end of this step.
    void destroy(Entity e) const { api_->destroy(api_->context, e); }

    // Sound (the entity's Audio Source component).
    void playSound() const { api_->sound(api_->context, entity_, 1); }
    void stopSound() const { api_->sound(api_->context, entity_, 0); }
    [[nodiscard]] bool soundPlaying() const { return api_->sound(api_->context, entity_, 2) != 0; }
    void playSoundOn(Entity e) const { api_->sound(api_->context, e, 1); }

    /// A number from the Script component's Parameters field ("speed=4, jump=6").
    [[nodiscard]] float param(std::string_view key, float fallback) const {
        return static_cast<float>(api_->parameter(api_->context, entity_, std::string(key).c_str(), fallback));
    }

    [[nodiscard]] bool keyDown(Key key) const { return api_->keyDown(api_->context, static_cast<int>(key)) != 0; }
    [[nodiscard]] bool mouseDown(MouseButton button) const {
        return api_->mouseDown(api_->context, static_cast<int>(button)) != 0;
    }
    /// Pixels the mouse moved this frame.
    [[nodiscard]] Vec3 mouseDelta() const {
        float xy[2] = {0.0f, 0.0f};
        api_->mouseDelta(api_->context, xy);
        return {xy[0], xy[1], 0.0f};
    }
    [[nodiscard]] double time() const { return api_->time(api_->context); }
    void log(std::string_view message) const { api_->log(api_->context, 0, std::string(message).c_str()); }
    void warn(std::string_view message) const { api_->log(api_->context, 1, std::string(message).c_str()); }

private:
    friend struct detail::Access;
    [[nodiscard]] Vec3 get(Entity e, int which) const {
        float v[3] = {0.0f, 0.0f, 0.0f};
        api_->getTransform(api_->context, e, which, v);
        return {v[0], v[1], v[2]};
    }
    void push(Vec3 f, int impulse) const {
        const float xyz[3] = {f.x, f.y, f.z};
        api_->addForce(api_->context, entity_, xyz, impulse);
    }
    void set(Entity e, int which, Vec3 value) const {
        const float v[3] = {value.x, value.y, value.z};
        api_->setTransform(api_->context, e, which, v);
    }

    const EngineApi* api_ = nullptr;
    Entity entity_ = kNoEntity;
};

namespace detail {
struct Access {
    static void bind(Behaviour& b, const EngineApi* api, Entity e) {
        b.api_ = api;
        b.entity_ = e;
    }
};
} // namespace detail

} // namespace lumora::script

/// Registers a behaviour class under its name (the Script component's "Behaviour" field).
#define LUMORA_BEHAVIOUR(Type)                                                                     \
    static const ::lumora::script::detail::Registrar lumoraBehaviourRegistrar_##Type(              \
        #Type, []() -> ::lumora::script::Behaviour* { return new Type(); })
