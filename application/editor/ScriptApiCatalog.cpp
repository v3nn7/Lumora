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
    // Looks
    {S::Behaviour, "setMaterial", "bool setMaterial(path)", "Use a .lmat material (\"\" = own settings)"},
    {S::Behaviour, "setMaterialOf", "bool setMaterialOf(Entity e, path)", "Another entity's material"},
    {S::Behaviour, "setColor", "bool setColor(Vec3 rgb)", "Mesh Renderer base colour (0..1)"},
    // Debug drawing and timers
    {S::Behaviour, "debugLine", "void debugLine(Vec3 from, Vec3 to, Vec3 color)", "Debug line on top of the scene"},
    {S::Behaviour, "debugBox", "void debugBox(Vec3 center, Vec3 halfSize, Vec3 color)", "Debug box outline"},
    {S::Behaviour, "debugSphere", "void debugSphere(Vec3 center, float radius, Vec3 color)", "Debug sphere outline"},
    {S::Behaviour, "after", "TimerId after(float seconds, [this] { ... })", "Run code once, later"},
    {S::Behaviour, "every", "TimerId every(float seconds, [this] { ... })", "Run code repeatedly"},
    {S::Behaviour, "cancelTimer", "void cancelTimer(TimerId id)", "Stop a timer"},
    // Particles
    {S::Behaviour, "emitParticles", "void emitParticles(int count)", "Particle Emitter: a burst now"},
    {S::Behaviour, "setParticlesPlaying", "void setParticlesPlaying(bool playing)", "Start or stop emitting"},
    {S::Behaviour, "particleCount", "int particleCount()", "Particles alive"},
    // Game UI and input edges
    {S::Behaviour, "button", "bool button(text, x, y, w, h)", "Game UI button: true when clicked"},
    {S::Behaviour, "checkbox", "bool checkbox(text, bool& value, x, y, w = 260, h = 40)", "Flips value when clicked"},
    {S::Behaviour, "slider", "bool slider(text, float& value, min, max, x, y, w = 320, h = 40)", "Drag to change value"},
    {S::Behaviour, "panel", "void panel(x, y, w, h)", "Dark menu background"},
    {S::Behaviour, "uiStyle", "void uiStyle(Vec3 accent, float fontSize = 22)", "Look of the game UI"},
    {S::Behaviour, "drawTextCentered", "void drawTextCentered(text, centerX, y, size = 24, color, alpha)", "HUD text centred on x"},
    {S::Behaviour, "drawTextRight", "void drawTextRight(text, rightX, y, size = 24, color, alpha)", "HUD text ending at x"},
    {S::Behaviour, "mousePosition", "Vec3 mousePosition()", "Mouse in HUD pixels"},
    {S::Behaviour, "mousePressed", "bool mousePressed(MouseButton b)", "Went down since the last step"},
    {S::Behaviour, "mouseReleased", "bool mouseReleased(MouseButton b)", "Came up since the last step"},
    {S::Behaviour, "keyPressed", "bool keyPressed(Key key)", "Pressed since the last step (once)"},
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
    {S::Key, "Enter", "Key::Enter", ""},
    {S::Key, "Tab", "Key::Tab", ""},
    {S::Key, "Backspace", "Key::Backspace", ""},
    {S::Key, "B", "Key::B", ""},
    {S::Key, "C", "Key::C", ""},
    {S::Key, "G", "Key::G", ""},
    {S::Key, "H", "Key::H", ""},
    {S::Key, "I", "Key::I", ""},
    {S::Key, "J", "Key::J", ""},
    {S::Key, "K", "Key::K", ""},
    {S::Key, "L", "Key::L", ""},
    {S::Key, "M", "Key::M", ""},
    {S::Key, "N", "Key::N", ""},
    {S::Key, "O", "Key::O", ""},
    {S::Key, "P", "Key::P", ""},
    {S::Key, "R", "Key::R", ""},
    {S::Key, "T", "Key::T", ""},
    {S::Key, "U", "Key::U", ""},
    {S::Key, "V", "Key::V", ""},
    {S::Key, "X", "Key::X", ""},
    {S::Key, "Y", "Key::Y", ""},
    {S::Key, "Z", "Key::Z", ""},
    {S::Key, "Num0", "Key::Num0", ""},
    {S::Key, "Num1", "Key::Num1", ""},
    {S::Key, "Num2", "Key::Num2", ""},
    {S::Key, "Num3", "Key::Num3", ""},
    {S::Key, "Num4", "Key::Num4", ""},
    {S::Key, "Num5", "Key::Num5", ""},
    {S::Key, "Num6", "Key::Num6", ""},
    {S::Key, "Num7", "Key::Num7", ""},
    {S::Key, "Num8", "Key::Num8", ""},
    {S::Key, "Num9", "Key::Num9", ""},
    {S::MouseButton, "Left", "MouseButton::Left", ""}, {S::MouseButton, "Right", "MouseButton::Right", ""},
    {S::MouseButton, "Middle", "MouseButton::Middle", ""},
};

} // namespace

std::span<const ScriptApiEntry> scriptApi() { return kApi; }

} // namespace lumora::editor
