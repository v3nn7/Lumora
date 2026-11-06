#include "editor/ScriptApiCatalog.hpp"

namespace lumora::editor {

namespace {

using S = ScriptApiEntry::Scope;

constexpr ScriptApiEntry kApi[] = {
    // Behaviour overrides
    {S::Behaviour, "start", "void start() override", "Runs once when play begins (or after a reload)"},
    {S::Behaviour, "update", "void update(float dt) override", "Runs every fixed step (dt in seconds)"},
    {S::Behaviour, "onCollision", "void onCollision(Entity other) override", "This entity started touching another"},
    // Entities
    {S::Behaviour, "entity", "Entity entity()", "This behaviour's entity"},
    {S::Behaviour, "name", "std::string name(Entity e = this)", "Name of an entity"},
    {S::Behaviour, "find", "Entity find(name)", "First entity with this name, or kNoEntity"},
    {S::Behaviour, "findAll", "std::vector<Entity> findAll(name)", "Every entity with this name"},
    {S::Behaviour, "clone", "Entity clone(Entity source)", "Copy an entity with its children and scripts"},
    {S::Behaviour, "spawn", "Entity spawn(prefab, Vec3 position)", "Add a copy of a .lprefab"},
    {S::Behaviour, "destroy", "void destroy(Entity e)", "Remove an entity at the end of the step"},
    {S::Behaviour, "loadScene", "bool loadScene(path)", "Switch to another .lscene at the end of the step"},
    // Transform
    {S::Behaviour, "position", "Vec3 position()", "This entity's position (m)"},
    {S::Behaviour, "rotation", "Vec3 rotation()", "This entity's rotation (Euler degrees)"},
    {S::Behaviour, "scale", "Vec3 scale()", "This entity's scale"},
    {S::Behaviour, "setPosition", "void setPosition(Vec3 v)", "Move this entity"},
    {S::Behaviour, "setRotation", "void setRotation(Vec3 degrees)", "Turn this entity"},
    {S::Behaviour, "setScale", "void setScale(Vec3 v)", "Resize this entity"},
    {S::Behaviour, "positionOf", "Vec3 positionOf(Entity e)", "Another entity's position"},
    {S::Behaviour, "setPositionOf", "void setPositionOf(Entity e, Vec3 v)", "Move another entity"},
    // Physics
    {S::Behaviour, "velocity", "Vec3 velocity()", "Velocity of the dynamic Rigid Body (m/s)"},
    {S::Behaviour, "setVelocity", "void setVelocity(Vec3 v)", "Set the Rigid Body's velocity"},
    {S::Behaviour, "addForce", "void addForce(Vec3 newtons)", "Push the Rigid Body this step"},
    {S::Behaviour, "addImpulse", "void addImpulse(Vec3 newtonSeconds)", "Kick the Rigid Body"},
    {S::Behaviour, "raycast", "bool raycast(Vec3 origin, Vec3 direction, float maxDistance, RayHit& hit)",
     "Closest collider along a ray"},
    // Character
    {S::Behaviour, "move", "void move(Vec3 velocity)", "Character Controller: walk (m/s)"},
    {S::Behaviour, "jump", "bool jump(float speed)", "Character Controller: jump if standing"},
    {S::Behaviour, "grounded", "bool grounded()", "Character Controller: standing on something"},
    // HUD
    {S::Behaviour, "drawRect", "void drawRect(x, y, w, h, Vec3 color, float alpha = 1)", "HUD rectangle (pixels)"},
    {S::Behaviour, "drawText", "void drawText(text, x, y, size = 24, Vec3 color = white, alpha = 1)", "HUD text"},
    {S::Behaviour, "screenSize", "Vec3 screenSize()", "Game view size in pixels (x, y)"},
    // Animation and sound
    {S::Behaviour, "playAnimation", "bool playAnimation(clip = \"\")", "Start an Animator clip"},
    {S::Behaviour, "stopAnimation", "void stopAnimation()", "Stop the Animator"},
    {S::Behaviour, "animationPlaying", "bool animationPlaying()", "Is a clip playing"},
    {S::Behaviour, "playSound", "void playSound()", "Play this entity's Audio Source"},
    {S::Behaviour, "stopSound", "void stopSound()", "Stop this entity's Audio Source"},
    {S::Behaviour, "soundPlaying", "bool soundPlaying()", "Is the Audio Source playing"},
    {S::Behaviour, "playSoundOn", "void playSoundOn(Entity e)", "Play another entity's Audio Source"},
    // Input, time, parameters, log
    {S::Behaviour, "keyDown", "bool keyDown(Key key)", "Is a key held (Key::W ...)"},
    {S::Behaviour, "mouseDown", "bool mouseDown(MouseButton button)", "Is a mouse button held"},
    {S::Behaviour, "mouseDelta", "Vec3 mouseDelta()", "Pixels the mouse moved this frame"},
    {S::Behaviour, "time", "double time()", "Simulated seconds since play started"},
    {S::Behaviour, "param", "float param(key, float fallback)", "Number from the Script's Parameters field"},
    {S::Behaviour, "log", "void log(message)", "Write to the Console"},
    {S::Behaviour, "warn", "void warn(message)", "Write a warning to the Console"},
    // Types
    {S::Type, "Behaviour", "class Behaviour", "Base class of gameplay behaviours"},
    {S::Type, "Vec3", "struct Vec3 { x, y, z }", "3D vector: + - * length()"},
    {S::Type, "Entity", "Entity", "An entity id"},
    {S::Type, "kNoEntity", "kNoEntity", "No entity"},
    {S::Type, "RayHit", "struct RayHit", "raycast result: entity, point, normal, distance"},
    {S::Type, "Key", "enum class Key", "Keyboard keys"},
    {S::Type, "MouseButton", "enum class MouseButton", "Left, Right, Middle"},
    {S::Type, "LUMORA_BEHAVIOUR", "LUMORA_BEHAVIOUR(Type);", "Register a behaviour class"},
    // Members
    {S::Member, "x", "float x", "Vec3"},
    {S::Member, "y", "float y", "Vec3"},
    {S::Member, "z", "float z", "Vec3"},
    {S::Member, "length", "float length()", "Vec3 length"},
    {S::Member, "entity", "Entity entity", "RayHit: what was hit"},
    {S::Member, "point", "Vec3 point", "RayHit: where"},
    {S::Member, "normal", "Vec3 normal", "RayHit: surface normal"},
    {S::Member, "distance", "float distance", "RayHit: distance along the ray"},
    // Keys
    {S::Key, "W", "Key::W", ""}, {S::Key, "A", "Key::A", ""}, {S::Key, "S", "Key::S", ""},
    {S::Key, "D", "Key::D", ""}, {S::Key, "Q", "Key::Q", ""}, {S::Key, "E", "Key::E", ""},
    {S::Key, "F", "Key::F", ""}, {S::Key, "Up", "Key::Up", ""}, {S::Key, "Down", "Key::Down", ""},
    {S::Key, "Left", "Key::Left", ""}, {S::Key, "Right", "Key::Right", ""}, {S::Key, "Space", "Key::Space", ""},
    {S::Key, "LeftShift", "Key::LeftShift", ""}, {S::Key, "LeftCtrl", "Key::LeftCtrl", ""},
    {S::Key, "LeftAlt", "Key::LeftAlt", ""}, {S::Key, "Escape", "Key::Escape", ""},
    {S::MouseButton, "Left", "MouseButton::Left", ""}, {S::MouseButton, "Right", "MouseButton::Right", ""},
    {S::MouseButton, "Middle", "MouseButton::Middle", ""},
};

} // namespace

std::span<const ScriptApiEntry> scriptApi() { return kApi; }

} // namespace lumora::editor
