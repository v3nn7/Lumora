#pragma once

#include "assets/Primitives.hpp"
#include "core/Uuid.hpp"

#include <entt/entity/entity.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lumora {

/// Persistent identity. Every scene entity has exactly one.
struct IdComponent {
    Uuid id;
};

struct NameComponent {
    std::string name;
};

/// Local transform relative to the parent. Rotation is stored as Euler angles in degrees
/// (applied yaw(Y) * pitch(X) * roll(Z)), which keeps editing and serialization lossless.
struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotationDeg{0.0f};
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 localMatrix() const;
};

/// Cached world matrix, refreshed by Scene::updateWorldTransforms().
struct WorldTransformComponent {
    glm::mat4 matrix{1.0f};
};

struct HierarchyComponent {
    entt::entity parent{entt::null};
    std::vector<entt::entity> children;
};

/// Procedural world-space surface pattern multiplied into the base colour (prototyping material).
enum class SurfacePattern : std::int32_t { None = 0, Grid = 1, Checker = 2 };

struct MeshRendererComponent {
    MeshPrimitive mesh = MeshPrimitive::Cube;
    /// Imported mesh ("Content/Models/X.glb#0"); overrides `mesh` when set.
    std::string meshAsset;
    glm::vec3 baseColor{0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive{0.0f};
    float emissiveStrength = 0.0f;
    SurfacePattern pattern = SurfacePattern::None;
    // Texture maps (project-relative keys, see AssetLibrary). Empty = none.
    std::string baseColorTexture;
    std::string metallicRoughnessTexture;  // glTF packing: G = roughness, B = metallic
    std::string normalTexture;
    std::string emissiveTexture;
    /// Material asset (.lmat, project-relative). When set it replaces the settings above, and
    /// every entity using it changes when the file does.
    std::string material;
    bool visible = true;
    /// Cut-out surfaces (leaves, fences, hair cards): where the base colour map's alpha is below
    /// this, nothing is drawn and no shadow is cast. 0 = opaque; -1 = what the model's material
    /// says (glTF alphaMode), the default.
    float alphaCutoff = -1.0f;
    /// Drawn from behind too (thin things: leaves, cloth, paper). Imported models' two-sided
    /// materials are always drawn so.
    bool doubleSided = false;
};

enum class LightType : std::int32_t { Directional = 0, Point = 1, Spot = 2 };

/// Directional and spot lights shine along the entity's local -Z axis; intensity in lux-like units.
struct LightComponent {
    LightType type = LightType::Directional;
    glm::vec3 color{1.0f};
    float intensity = 3.0f;
    float range = 10.0f;
    float spotAngleDeg = 35.0f;  // spot: half-angle of the cone
    float spotSoftness = 0.25f;  // spot: fraction of the cone that fades out at its edge
    /// Spot and point lights: shadows from a tile of the local shadow atlas (16 tiles; a spot
    /// takes one, a point light six; the lights nearest the camera get them first).
    bool castShadows = false;
};

struct CameraComponent {
    float fovDeg = 60.0f;
    float nearPlane = 0.05f;
    bool primary = true;
};

/// A walking character (Jolt CharacterVirtual): an upright capsule standing on the entity's
/// origin that slides along walls, climbs steps and slopes, falls with gravity and pushes
/// dynamic bodies. Scripts steer it with move() and jump(); the physics writes its position.
struct CharacterControllerComponent {
    float height = 1.8f;        // total height (m)
    float radius = 0.3f;
    float maxSlopeDeg = 50.0f;  // steeper ground is a wall
    float stepHeight = 0.35f;   // steps up to this height are climbed
    float gravityFactor = 1.0f;
};

enum class ParticleShape : std::int32_t { Point = 0, Sphere = 1, Box = 2 };
enum class ParticleBlend : std::int32_t { Alpha = 0, Additive = 1 };

/// Emits camera-facing particles (smoke, sparks, fire, dust). They leave along the entity's
/// local +Y axis within a cone, fall with gravity, slow with drag, and fade from the start to
/// the end size and colour over their life. Simulated in play mode and previewed in the editor.
struct ParticleEmitterComponent {
    bool playing = true;          // emitting continuously at `rate`
    float rate = 20.0f;           // particles per second
    std::int32_t burst = 0;       // particles emitted at once when it starts
    std::int32_t maxParticles = 500;
    float lifetime = 2.0f;        // seconds
    float lifetimeVariation = 0.3f;  // +- fraction
    float speed = 2.0f;           // m/s at emission
    float speedVariation = 0.3f;
    float spreadDeg = 25.0f;      // half-angle of the emission cone around +Y
    ParticleShape shape = ParticleShape::Point;
    float shapeSize = 0.2f;       // sphere radius / box half-size
    float gravity = 0.0f;         // multiples of 9.81 m/s^2 downwards (negative: rise)
    float drag = 0.0f;            // per second
    float startSize = 0.25f;      // metres
    float endSize = 0.6f;
    glm::vec3 startColor{1.0f};
    glm::vec3 endColor{1.0f};
    float startAlpha = 1.0f;
    float endAlpha = 0.0f;
    float intensity = 1.0f;       // brightness (> 1 glows with bloom)
    ParticleBlend blend = ParticleBlend::Alpha;
    bool worldSpace = true;       // false: particles move with the entity
};

/// Projects a texture down the entity's local -Y axis onto the surfaces inside its box (the
/// entity's unit cube, sized by its scale), multiplying their colour: dirt, cracks, paint, stains.
struct DecalComponent {
    std::string texture;        // project-relative image; empty = a plain colour
    glm::vec3 color{0.25f};
    float opacity = 1.0f;
};

/// Ground from a heightmap (a grayscale image, white = `height`) or generated hills, centred on
/// the entity. Solid in play mode and walkable for navigation. Grass on flat ground blends into
/// rock on slopes.
struct TerrainComponent {
    float size = 200.0f;          // metres per side
    float height = 25.0f;         // metres from black to white
    int resolution = 129;         // vertices per side
    std::string heightmap;        // project-relative image; empty = generated hills
    int seed = 1;
    float hillSize = 60.0f;       // metres across a typical hill (generated)
    glm::vec3 grassColor{0.28f, 0.42f, 0.16f};
    glm::vec3 rockColor{0.40f, 0.38f, 0.35f};
    std::string texture;          // tiling ground texture, multiplied (optional)
    float textureTiling = 4.0f;   // metres per repeat
};

/// A water surface at the entity's height: waves, a colour that deepens with depth, reflections
/// of the sky and the sun. Dynamic bodies in it float (Buoyancy > 0).
struct WaterComponent {
    float width = 100.0f;   // X, metres
    float length = 100.0f;  // Z, metres
    glm::vec3 shallowColor{0.10f, 0.42f, 0.45f};
    glm::vec3 deepColor{0.01f, 0.08f, 0.14f};
    float clarity = 3.0f;      // metres one sees down
    float waveHeight = 0.2f;
    float waveLength = 8.0f;
    float waveSpeed = 1.0f;
    float buoyancy = 1.2f;     // 1 = floats level with the surface when as dense as water
};

/// Scatters copies of a mesh around the entity (grass, bushes, rocks, trees), on the terrain
/// below when there is one. Drawn with GPU instancing: thousands of copies stay cheap.
struct FoliageComponent {
    MeshPrimitive mesh = MeshPrimitive::Cube;
    std::string meshAsset;  // overrides `mesh`
    std::string material;   // .lmat; empty = Color
    glm::vec3 color{0.25f, 0.45f, 0.18f};
    int count = 500;
    float radius = 20.0f;   // scattered within this distance (a square) of the entity
    int seed = 1;
    float minScale = 0.7f;
    float maxScale = 1.3f;
    bool randomRotation = true;
    bool onTerrain = true;  // snap to a Terrain below
    float maxSlope = 35.0f; // degrees; steeper ground stays bare (on a terrain)
};

/// Plays a timeline (.ltimeline: a cutscene or scripted sequence) moving and showing entities,
/// starting animations and sounds and sending events to scripts.
struct TimelineComponent {
    std::string timeline;  // project-relative .ltimeline
    bool playOnStart = true;
    bool loop = false;
    float speed = 1.0f;
};

/// Run-time state of an emitter (not saved): the live particles.
struct ParticleStateComponent {
    struct Particle {
        glm::vec3 position;  // world (or emitter-local when not worldSpace)
        glm::vec3 velocity;
        float age;
        float life;
    };
    std::vector<Particle> particles;
    float pending = 0.0f;     // fractional particles owed by the rate
    std::uint32_t rng = 0x9e3779b9u;
    bool started = false;     // burst done
    std::int32_t requested = 0;  // extra particles asked for (scripts)
};

enum class JointType : std::int32_t { Fixed = 0, Hinge = 1, Ball = 2, Slider = 3, Distance = 4 };

/// Ties this entity's rigid body to another entity's (by name) or to the world: doors (hinge),
/// chains and pendulums (ball), drawers and pistons (slider), ropes and springs (distance),
/// welded parts (fixed). Needs a Collider and a Rigid Body on this entity.
struct JointComponent {
    JointType type = JointType::Hinge;
    std::string connectedTo;          // entity name; empty = fixed to the world
    glm::vec3 anchor{0.0f};           // pivot in this entity's local space
    glm::vec3 axis{0.0f, 1.0f, 0.0f}; // hinge / slider axis in local space
    bool limits = false;
    float minLimit = -45.0f;          // hinge: degrees, slider: metres
    float maxLimit = 45.0f;
    float minDistance = 0.0f;         // distance joint
    float maxDistance = 1.0f;
    float spring = 0.0f;              // distance limits: frequency in Hz (0 = rigid)
    float motorSpeed = 0.0f;          // hinge: degrees/s, slider: m/s
    float motorForce = 0.0f;          // torque (N m) / force (N); 0 = no motor
};

/// Turns an imported, skinned model into a ragdoll: every bone of its skeleton becomes a body
/// (a capsule to its child bones) linked to its parent by a swing-twist joint. Off until
/// activated (here or by a script: setRagdoll(true)); the animation stops when it activates.
struct RagdollComponent {
    bool active = false;
    float mass = 70.0f;  // kg, shared by the bones
};

/// A car on its rigid body: four wheels at the corners of its box collider with suspension, an
/// engine, gears and steering. Scripts drive it with drive(throttle, steering, brake). Child
/// entities named "Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR" follow the wheels.
struct VehicleComponent {
    float wheelRadius = 0.35f;
    float wheelWidth = 0.25f;
    float suspensionLength = 0.3f;
    float engineTorque = 500.0f;  // N m
    float maxSteerDeg = 30.0f;
};

/// An agent that walks on the scene's navigation mesh (built from the static colliders): scripts
/// send it somewhere with moveTo() and it finds the way around obstacles and other agents. With
/// a Character Controller it walks through the physics; without, it is moved directly.
struct NavAgentComponent {
    float speed = 3.5f;          // m/s
    float acceleration = 10.0f;  // m/s^2
    float radius = 0.4f;         // keeps this far from walls and other agents
    float height = 1.8f;
};

/// Moves the entity back and forth: position = start + offset * sin(2 pi t / period), while the
/// simulation runs. With a kinematic Rigid Body and a Collider it is a moving platform that
/// carries characters standing on it.
struct OscillatorComponent {
    glm::vec3 offset{0.0f, 2.0f, 0.0f};  // furthest displacement from the start (m)
    float period = 4.0f;                 // seconds per full cycle
    float phase = 0.0f;                  // 0..1, where in the cycle it starts
};

/// Runs a C++ behaviour from the project's game library (Scripts/) while the simulation runs.
struct ScriptComponent {
    std::string behaviour;   // class name registered with LUMORA_BEHAVIOUR
    std::string parameters;  // "speed=4, jump=6": numbers the behaviour reads with param()
};

/// Plays a sound file (project-relative WAV, FLAC or MP3) while the simulation runs, in 3D at the
/// entity (spatial) or flat. Scripts start and stop it with playSound() / stopSound().
/// Mixer groups every sound plays through (all of them below Master).
enum class AudioBus : int { Music = 0, Sfx, Ui, Voice, Ambience };

struct AudioSourceComponent {
    std::string clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool playOnStart = true;
    bool spatial = true;
    float minDistance = 1.0f;   // full volume up to here (metres)
    float maxDistance = 50.0f;  // no further attenuation beyond
    AudioBus bus = AudioBus::Sfx;  // mixer group; scripts set each bus's volume
};

/// Plays a glTF model's animation clip on the entities instantiated from that model (the
/// entity carrying this is the model's root; its descendants carry ModelNodeComponent).
struct AnimatorComponent {
    std::string model;         // project-relative .glb/.gltf the clips come from
    std::string clip;          // clip name; empty = the model's first clip
    float speed = 1.0f;
    bool loop = true;
    bool playOnStart = true;
    std::string controller;    // optional project-relative .lanim state machine that picks the clip
};

/// The mesh on this entity is deformed by a skin of its model: the joints are the entities
/// mirroring the skin's joint nodes (ModelNodeComponent) under the same model root.
struct SkinComponent {
    int skin = -1;  // index into the model's skins (the model is the one of MeshRenderer::meshAsset)
};

/// Which node of the source model an entity mirrors (set by instantiateModel; used by animation).
struct ModelNodeComponent {
    int node = -1;
};

/// Built-in gameplay behaviour: spins the entity while the simulation runs.
struct RotatorComponent {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float speedDeg = 45.0f;
};

enum class BodyMotion : std::int32_t { Static = 0, Kinematic = 1, Dynamic = 2 };

/// Simulated body. Without a RigidBody, an entity with a Collider is a static obstacle.
/// Kinematic bodies follow the entity transform (e.g. animated by a Rotator) and push dynamic ones.
struct RigidBodyComponent {
    BodyMotion motion = BodyMotion::Dynamic;
    float mass = 1.0f;  // kg; inertia is derived from the collider shape
    float friction = 0.5f;
    float restitution = 0.1f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    float gravityFactor = 1.0f;
};

enum class ColliderShape : std::int32_t { Box = 0, Sphere = 1, Capsule = 2 };

/// Collision shape in the entity's local space; the entity's world scale is applied.
struct ColliderComponent {
    ColliderShape shape = ColliderShape::Box;
    glm::vec3 size{1.0f};    // box full extents
    float radius = 0.5f;     // sphere / capsule
    float height = 1.0f;     // capsule cylinder part (total = height + 2 * radius)
    glm::vec3 offset{0.0f};  // local-space centre offset
    /// A trigger does not block anything; touching it is reported to scripts (onCollision).
    bool isTrigger = false;
};

/// Scene-wide settings stored alongside entities.
struct EnvironmentSettings {
    glm::vec3 skyZenithColor{0.07f, 0.20f, 0.56f};
    glm::vec3 skyHorizonColor{0.56f, 0.67f, 0.82f};
    glm::vec3 groundColor{0.16f, 0.14f, 0.12f};
    float ambientIntensity = 0.5f;
    float exposure = 0.9f;
    float fogDensity = 0.008f;        // extinction per metre at height 0
    float fogHeightFalloff = 0.12f;   // fog thins out with altitude
    float bloomIntensity = 0.05f;     // fraction of the blurred image mixed back in
    float vignette = 0.35f;
    // Image-based lighting: an equirectangular .hdr replaces the procedural sky and lights the
    // scene (reflections and ambient). Empty = procedural sky.
    std::string skyTexture;
    float skyIntensity = 1.0f;
    float skyRotationDeg = 0.0f;  // turns the environment around the vertical axis
    // Colour grading, after tone mapping.
    float contrast = 1.0f;
    float saturation = 1.0f;
    float temperature = 0.0f;  // -1 cool .. 1 warm
    float tint = 0.0f;         // -1 green .. 1 magenta
    // Depth of field: sharp around the focus distance, blurred beyond the focus range.
    float dofBlur = 0.0f;           // largest blur radius in pixels (0 = off)
    float focusDistance = 10.0f;    // metres from the camera
    float focusRange = 6.0f;        // metres around it that stay sharp
    /// Light shafts from the sun through gaps in the scene (0 = off).
    float sunShafts = 0.0f;
    /// Sun shadows from a virtual shadow map: sharp up close, cached between frames. Off = the
    /// classic four cascades.
    bool virtualShadowMaps = true;
};

} // namespace lumora
