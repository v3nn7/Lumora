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

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#define LUMORA_SCRIPT_ABI 3u

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
    Enter, Tab, Backspace,
    B, C, G, H, I, J, K, L, M, N, O, P, R, T, U, V, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
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
    int (*animation)(void* context, Entity entity, const char* clip, int command);  // 0 stop, 1 play, 2 playing
    int (*character)(void* context, Entity entity, int command, const float* xyz);  // 0 move, 1 jump, 2 grounded
    void (*drawRect)(void* context, const float* xywh, const float* rgba);
    void (*drawText)(void* context, const char* utf8, const float* xySize, const float* rgba);
    void (*screenSize)(void* context, float* wh);
    int (*findAll)(void* context, const char* name, Entity* out, int capacity);  // returns the total
    // ABI 3
    int (*loadScene)(void* context, const char* path);  // queued for the end of the step; 0 if invalid
    int (*setMaterial)(void* context, Entity entity, const char* path);  // "" = own settings; 0 if no mesh
    int (*setColor)(void* context, Entity entity, const float* rgb);       // Mesh Renderer base colour
    void (*mousePosition)(void* context, float* xy);                        // HUD pixels
    int (*inputEdge)(void* context, int kind, int code);  // this step: 0 mouse pressed, 1 released, 2 key pressed
    // kind 0 button, 1 checkbox, 2 slider (value in/out, range = min, max), 3 panel, 4 label (value = align)
    int (*ui)(void* context, int kind, const char* label, const float* rect, float* value, const float* range);
    void (*uiStyle)(void* context, const float* accentRgb, float fontSize);
    void (*drawTextAligned)(void* context, const char* utf8, const float* xyWidthSize, int align, const float* rgba);
    int (*particles)(void* context, Entity entity, int command, int count);  // 0 stop, 1 play, 2 burst, 3 alive
    void (*debugDraw)(void* context, int kind, const float* a, const float* b, const float* rgba);  // 0 line, 1 box, 2 sphere
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

    /// Every entity with this name.
    [[nodiscard]] std::vector<Entity> findAll(std::string_view entityName) const {
        const std::string key(entityName);
        std::vector<Entity> out(16);
        int total = api_->findAll(api_->context, key.c_str(), out.data(), static_cast<int>(out.size()));
        if (total > static_cast<int>(out.size())) {
            out.resize(static_cast<std::size_t>(total));
            total = api_->findAll(api_->context, key.c_str(), out.data(), total);
        }
        out.resize(static_cast<std::size_t>(std::min<int>(total, static_cast<int>(out.size()))));
        return out;
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

    // Character Controller component: walking, jumping, standing on things.
    /// Walking velocity (m/s, world space; only x and z count - gravity and jumps own y).
    void move(Vec3 velocity) const {
        const float xyz[3] = {velocity.x, velocity.y, velocity.z};
        api_->character(api_->context, entity_, 0, xyz);
    }
    /// Jumps with this upward speed (m/s); false if not standing on the ground.
    bool jump(float speed) const {
        const float xyz[3] = {speed, 0.0f, 0.0f};
        return api_->character(api_->context, entity_, 1, xyz) != 0;
    }
    [[nodiscard]] bool grounded() const { return api_->character(api_->context, entity_, 2, nullptr) != 0; }

    // HUD: 2D drawing on top of the game, in pixels from the top-left corner. Draw it again
    // in every update(); it is cleared before each step. Colours are sRGB, alpha 0..1.
    void drawRect(float x, float y, float w, float h, Vec3 color, float alpha = 1.0f) const {
        const float r[4] = {x, y, w, h};
        const float c[4] = {color.x, color.y, color.z, alpha};
        api_->drawRect(api_->context, r, c);
    }
    void drawText(std::string_view text, float x, float y, float size = 24.0f, Vec3 color = Vec3{1, 1, 1},
                  float alpha = 1.0f) const {
        const float p[3] = {x, y, size};
        const float c[4] = {color.x, color.y, color.z, alpha};
        api_->drawText(api_->context, std::string(text).c_str(), p, c);
    }
    /// Text aligned in a box: centred (align 1) or right-aligned (align 2) within [x, x + width].
    void drawTextCentered(std::string_view text, float centerX, float y, float size = 24.0f, Vec3 color = Vec3{1, 1, 1},
                          float alpha = 1.0f) const {
        drawAligned(text, centerX, y, 0.0f, size, 1, color, alpha);
    }
    void drawTextRight(std::string_view text, float rightX, float y, float size = 24.0f, Vec3 color = Vec3{1, 1, 1},
                       float alpha = 1.0f) const {
        drawAligned(text, rightX, y, 0.0f, size, 2, color, alpha);
    }

    // Game UI: menus and settings. Call the widgets every update() you want them shown; each
    // draws itself and says whether it was used. Pixels from the top-left, like the HUD.
    /// True when clicked.
    bool button(std::string_view text, float x, float y, float w, float h) const {
        const float r[4] = {x, y, w, h};
        return api_->ui(api_->context, 0, std::string(text).c_str(), r, nullptr, nullptr) != 0;
    }
    /// Flips `value` when clicked; true then.
    bool checkbox(std::string_view text, bool& value, float x, float y, float w = 260.0f, float h = 40.0f) const {
        const float r[4] = {x, y, w, h};
        float v = value ? 1.0f : 0.0f;
        const bool clicked = api_->ui(api_->context, 1, std::string(text).c_str(), r, &v, nullptr) != 0;
        if (clicked) value = !value;
        return clicked;
    }
    /// Drag to change `value` within [min, max]; true while it changes.
    bool slider(std::string_view text, float& value, float min, float max, float x, float y, float w = 320.0f,
                float h = 40.0f) const {
        const float r[4] = {x, y, w, h};
        const float range[2] = {min, max};
        return api_->ui(api_->context, 2, std::string(text).c_str(), r, &value, range) != 0;
    }
    /// A dark background behind a menu.
    void panel(float x, float y, float w, float h) const {
        const float r[4] = {x, y, w, h};
        api_->ui(api_->context, 3, "", r, nullptr, nullptr);
    }
    /// Accent colour of buttons, checkboxes and sliders, and their text size.
    void uiStyle(Vec3 accent, float fontSize = 22.0f) const {
        const float c[3] = {accent.x, accent.y, accent.z};
        api_->uiStyle(api_->context, c, fontSize);
    }
    /// Mouse position in HUD pixels (x, y).
    [[nodiscard]] Vec3 mousePosition() const {
        float xy[2] = {0.0f, 0.0f};
        api_->mousePosition(api_->context, xy);
        return {xy[0], xy[1], 0.0f};
    }
    /// Went down / came up since the previous step (once per click).
    [[nodiscard]] bool mousePressed(MouseButton button) const {
        return api_->inputEdge(api_->context, 0, static_cast<int>(button)) != 0;
    }
    [[nodiscard]] bool mouseReleased(MouseButton button) const {
        return api_->inputEdge(api_->context, 1, static_cast<int>(button)) != 0;
    }
    /// Pressed since the previous step (once per press, unlike keyDown).
    [[nodiscard]] bool keyPressed(Key key) const { return api_->inputEdge(api_->context, 2, static_cast<int>(key)) != 0; }

    /// Size of the game's view in pixels (x = width, y = height).
    [[nodiscard]] Vec3 screenSize() const {
        float wh[2] = {0.0f, 0.0f};
        api_->screenSize(api_->context, wh);
        return {wh[0], wh[1], 0.0f};
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

    /// Switches to another scene (project-relative, e.g. "Content/Scenes/Level2.lscene") at the
    /// end of this step: every behaviour of this scene stops, the new scene's start. time()
    /// starts again from 0. False if the path is not a scene inside the project.
    bool loadScene(std::string_view path) const {
        return api_->loadScene(api_->context, std::string(path).c_str()) != 0;
    }

    // Animation (the entity's Animator component: clips of the glTF model it was made from).
    /// Starts a clip by name (empty = the current one) from the beginning.
    bool playAnimation(std::string_view clip = {}) const {
        return api_->animation(api_->context, entity_, std::string(clip).c_str(), 1) != 0;
    }
    void stopAnimation() const { api_->animation(api_->context, entity_, nullptr, 0); }
    [[nodiscard]] bool animationPlaying() const { return api_->animation(api_->context, entity_, nullptr, 2) != 0; }

    // Looks (the entity's Mesh Renderer).
    /// Uses a material asset (project-relative .lmat), or the renderer's own settings with "".
    bool setMaterial(std::string_view path) const { return setMaterialOf(entity_, path); }
    bool setMaterialOf(Entity e, std::string_view path) const {
        return api_->setMaterial(api_->context, e, std::string(path).c_str()) != 0;
    }
    /// Base colour (sRGB 0..1) of the renderer's own settings (not of a shared material).
    bool setColor(Vec3 rgb) const {
        const float c[3] = {rgb.x, rgb.y, rgb.z};
        return api_->setColor(api_->context, entity_, c) != 0;
    }

    // Debug drawing: shapes on top of the scene for this step (draw them in every update()).
    // Shown in the editor while playing, and in the player after F3.
    void debugLine(Vec3 from, Vec3 to, Vec3 color = Vec3{1.0f, 0.85f, 0.2f}) const { debug(0, from, to, color); }
    void debugBox(Vec3 center, Vec3 halfSize, Vec3 color = Vec3{0.3f, 0.9f, 0.4f}) const {
        debug(1, center, halfSize, color);
    }
    void debugSphere(Vec3 center, float radius, Vec3 color = Vec3{0.4f, 0.7f, 1.0f}) const {
        debug(2, center, Vec3{radius, 0.0f, 0.0f}, color);
    }

    // Timers: run code later or repeatedly (in game time, before update()). They belong to this
    // behaviour and stop with it.
    using TimerId = int;
    /// Calls `fn` once, `seconds` from now.
    TimerId after(float seconds, std::function<void()> fn) { return addTimer(seconds, 0.0f, std::move(fn)); }
    /// Calls `fn` every `seconds` (the first time `seconds` from now).
    TimerId every(float seconds, std::function<void()> fn) {
        return addTimer(seconds, seconds > 0.0f ? seconds : 1e-3f, std::move(fn));
    }
    void cancelTimer(TimerId id) {
        for (Timer& t : timers_) {
            if (t.id == id) t.id = 0;
        }
    }

    // Particles (the entity's Particle Emitter).
    /// Emits `count` particles at once (an explosion, a hit).
    void emitParticles(int count) const { api_->particles(api_->context, entity_, 2, count); }
    /// Starts or stops continuous emission (live particles finish their life).
    void setParticlesPlaying(bool playing) const { api_->particles(api_->context, entity_, playing ? 1 : 0, 0); }
    [[nodiscard]] int particleCount() const { return api_->particles(api_->context, entity_, 3, 0); }

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
    struct Timer {
        TimerId id;
        double due;
        float interval;  // 0 = once
        std::function<void()> fn;
    };
    TimerId addTimer(float seconds, float interval, std::function<void()> fn) {
        const TimerId id = nextTimer_++;
        timers_.push_back({id, clock_ + static_cast<double>(seconds > 0.0f ? seconds : 0.0f), interval, std::move(fn)});
        return id;
    }
    /// Advances this behaviour's timers by dt and runs the ones due (in due order).
    void tickTimers(float dt) {
        clock_ += static_cast<double>(dt);
        for (;;) {
            std::size_t next = timers_.size();
            for (std::size_t i = 0; i < timers_.size(); ++i) {
                if (timers_[i].id != 0 && timers_[i].due <= clock_ && (next == timers_.size() || timers_[i].due < timers_[next].due)) {
                    next = i;
                }
            }
            if (next == timers_.size()) break;
            std::function<void()> fn = timers_[next].fn;  // the callback may add or cancel timers
            if (timers_[next].interval > 0.0f) {
                timers_[next].due += static_cast<double>(timers_[next].interval);
            } else {
                timers_[next].id = 0;
            }
            fn();
        }
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [](const Timer& t) { return t.id == 0; }),
                      timers_.end());
    }
    void debug(int kind, Vec3 a, Vec3 b, Vec3 color) const {
        const float pa[3] = {a.x, a.y, a.z};
        const float pb[3] = {b.x, b.y, b.z};
        const float c[4] = {color.x, color.y, color.z, 1.0f};
        api_->debugDraw(api_->context, kind, pa, pb, c);
    }
    void drawAligned(std::string_view text, float x, float y, float w, float size, int align, Vec3 color,
                     float alpha) const {
        const float box[4] = {x, y, w, size};
        const float c[4] = {color.x, color.y, color.z, alpha};
        api_->drawTextAligned(api_->context, std::string(text).c_str(), box, align, c);
    }
    void set(Entity e, int which, Vec3 value) const {
        const float v[3] = {value.x, value.y, value.z};
        api_->setTransform(api_->context, e, which, v);
    }

    const EngineApi* api_ = nullptr;
    Entity entity_ = kNoEntity;
    std::vector<Timer> timers_;
    TimerId nextTimer_ = 1;
    double clock_ = 0.0;
};

namespace detail {
struct Access {
    static void bind(Behaviour& b, const EngineApi* api, Entity e) {
        b.api_ = api;
        b.entity_ = e;
    }
    static void tick(Behaviour& b, float dt) { b.tickTimers(dt); }
};
} // namespace detail

} // namespace lumora::script

/// Registers a behaviour class under its name (the Script component's "Behaviour" field).
#define LUMORA_BEHAVIOUR(Type)                                                                     \
    static const ::lumora::script::detail::Registrar lumoraBehaviourRegistrar_##Type(              \
        #Type, []() -> ::lumora::script::Behaviour* { return new Type(); })
