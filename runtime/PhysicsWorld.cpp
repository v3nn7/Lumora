#include "runtime/PhysicsWorld.hpp"

#include "core/Log.hpp"
#include "scene/Landscape.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/matrix.hpp>

// Jolt must be included with its configuration macros (exported by the Jolt target).
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <functional>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace lumora {

namespace {

// ---- One-time Jolt runtime setup (reference counted) ----------------------------------------

std::mutex gJoltMutex;
int gJoltUsers = 0;

void joltTrace(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log::debug("Physics", "{}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expression, const char* message, const char* file, JPH::uint line) {
    log::error("Physics", "Jolt assertion {} failed: {} ({}:{})", expression, message != nullptr ? message : "", file, line);
    return false;  // do not break into a debugger
}
#endif

void acquireJolt() {
    std::scoped_lock lock(gJoltMutex);
    if (gJoltUsers++ > 0) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = joltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void releaseJolt() {
    std::scoped_lock lock(gJoltMutex);
    if (--gJoltUsers > 0) return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// ---- Collision layers: static vs moving ---------------------------------------------------

namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::ObjectLayer kCount = 2;
} // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
} // namespace BroadPhaseLayers

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return !(a == Layers::kStatic && b == Layers::kStatic);
    }
};

class BroadPhaseLayerMapping final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::kStatic ? BroadPhaseLayers::kStatic : BroadPhaseLayers::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::kStatic ? "Static" : "Moving";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer == Layers::kMoving || broad == BroadPhaseLayers::kMoving;
    }
};

JPH::Vec3 toJolt(const glm::vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat toJolt(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }
glm::vec3 fromJolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat fromJolt(JPH::QuatArg q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }

struct Pose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

Pose decompose(const glm::mat4& world) {
    Pose p;
    p.position = glm::vec3(world[3]);
    glm::mat3 r(world);
    for (int i = 0; i < 3; ++i) {
        p.scale[i] = glm::length(r[i]);
        r[i] = p.scale[i] > 1e-8f ? r[i] / p.scale[i] : glm::vec3(0.0f);
    }
    p.rotation = glm::normalize(glm::quat_cast(r));
    return p;
}

/// A unit vector perpendicular to `v`.
glm::vec3 perpendicular(const glm::vec3& v) {
    const glm::vec3 other = std::abs(v.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(glm::cross(v, other));
}

/// Writes a world pose into an entity's local transform (parent space). Scale is kept.
void writeWorldPose(Scene& scene, entt::entity entity, glm::vec3 worldPos, glm::quat worldRot, const glm::mat4* parentWorld) {
    if (parentWorld != nullptr) {
        const Pose parentPose = decompose(*parentWorld);
        worldPos = glm::vec3(glm::inverse(*parentWorld) * glm::vec4(worldPos, 1.0f));
        worldRot = glm::inverse(parentPose.rotation) * worldRot;
    }
    auto& t = scene.registry().get<TransformComponent>(entity);
    t.position = worldPos;
    float y = 0.0f, x = 0.0f, z = 0.0f;
    glm::extractEulerAngleYXZ(glm::mat4_cast(glm::normalize(worldRot)), y, x, z);
    t.rotationDeg = glm::degrees(glm::vec3(x, y, z));
}

JPH::RefConst<JPH::Shape> makeShape(const ColliderComponent& c, const glm::vec3& scale) {
    const glm::vec3 s = glm::abs(scale);
    JPH::RefConst<JPH::Shape> shape;
    switch (c.shape) {
        case ColliderShape::Box: {
            const glm::vec3 half = glm::max(c.size * s * 0.5f, glm::vec3(1e-3f));
            const float convex = std::min(JPH::cDefaultConvexRadius, std::min({half.x, half.y, half.z}) * 0.5f);
            shape = new JPH::BoxShape(toJolt(half), convex);
            break;
        }
        case ColliderShape::Sphere:
            shape = new JPH::SphereShape(std::max(c.radius * std::max({s.x, s.y, s.z}), 1e-3f));
            break;
        case ColliderShape::Capsule: {
            const float radius = std::max(c.radius * std::max(s.x, s.z), 1e-3f);
            const float halfHeight = std::max(c.height * s.y * 0.5f, 1e-3f);
            shape = new JPH::CapsuleShape(halfHeight, radius);
            break;
        }
    }
    if (c.offset != glm::vec3(0.0f)) {
        JPH::RotatedTranslatedShapeSettings offset(toJolt(c.offset * s), JPH::Quat::sIdentity(), shape);
        auto result = offset.Create();
        if (result.IsValid()) shape = result.Get();
    }
    return shape;
}

} // namespace

/// Collects "started touching" pairs. Jolt calls it from its worker threads during Update().
class ContactCollector final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&,
                        JPH::ContactSettings&) override {
        std::scoped_lock lock(mutex);
        pairs.emplace_back(static_cast<entt::entity>(static_cast<std::uint32_t>(a.GetUserData())),
                           static_cast<entt::entity>(static_cast<std::uint32_t>(b.GetUserData())));
    }
    std::mutex mutex;
    std::vector<std::pair<entt::entity, entt::entity>> pairs;
};

/// Reports what characters start touching (bodies, triggers and other characters) into the same
/// list as body contacts. Characters are moved on the main thread, so no lock contention.
class CharacterContacts final : public JPH::CharacterContactListener {
public:
    explicit CharacterContacts(ContactCollector& out) : out_(out) {}
    void OnContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact,
                        JPH::CharacterContactSettings&) override {
        record(character->GetUserData(), contact.mUserData);
    }
    void OnCharacterContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact,
                                 JPH::CharacterContactSettings&) override {
        record(character->GetUserData(), contact.mUserData);
    }

private:
    void record(JPH::uint64 a, JPH::uint64 b) {
        std::scoped_lock lock(out_.mutex);
        out_.pairs.emplace_back(static_cast<entt::entity>(static_cast<std::uint32_t>(a)),
                                static_cast<entt::entity>(static_cast<std::uint32_t>(b)));
    }
    ContactCollector& out_;
};

struct PhysicsWorld::Impl {
    Scene& scene;
    BroadPhaseLayerMapping broadPhase;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectPairFilter objectPairs;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    JPH::PhysicsSystem system;
    ContactCollector contacts;
    CharacterContacts characterContacts{contacts};
    struct Body {
        JPH::BodyID id;
        BodyMotion motion;
    };
    std::unordered_map<entt::entity, Body> bodies;
    struct Character {
        JPH::Ref<JPH::CharacterVirtual> character;
        glm::vec3 desired{0.0f};  // horizontal walking velocity
        float vertical = 0.0f;    // gravity and jumps
        float pendingJump = 0.0f;
        float gravityFactor = 1.0f;
        float stepHeight = 0.35f;
        glm::vec3 lastWritten{0.0f};  // world position the physics last wrote (to notice teleports)
    };
    std::unordered_map<entt::entity, Character> characters;
    struct Joint {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        entt::entity other = entt::null;  // null = the world
        JointType type = JointType::Fixed;
        std::string connectedTo;
    };
    std::unordered_map<entt::entity, Joint> joints;
    struct Vehicle {
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::Ref<JPH::VehicleCollisionTester> tester;
        std::array<entt::entity, 4> wheels{entt::null, entt::null, entt::null, entt::null};
    };
    std::unordered_map<entt::entity, Vehicle> vehicles;
    struct RagdollInstance {
        JPH::Ref<JPH::Ragdoll> ragdoll;
        std::vector<entt::entity> bones;  // in skeleton order (parents first)
        std::vector<glm::vec3> scales;    // world scale of each bone
    };
    std::unordered_map<entt::entity, RagdollInstance> ragdolls;
    JPH::CollisionGroup::GroupID nextRagdollGroup = 1;
    std::filesystem::path projectRoot;
    struct TerrainBody {
        JPH::BodyID id;
        std::uint64_t key = 0;
    };
    std::unordered_map<entt::entity, TerrainBody> terrains;

    static std::uint64_t terrainBodyKey(const TerrainComponent& t, const glm::mat4& world, const std::filesystem::path& folder) {
        std::uint64_t h = terrainKey(t, folder);
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) h = h * 1099511628211ull ^ std::hash<float>{}(world[c][r]);
        }
        return h;
    }

    void removeTerrain(const TerrainBody& t) {
        JPH::BodyInterface& bi = system.GetBodyInterface();
        bi.RemoveBody(t.id);
        bi.DestroyBody(t.id);
    }

    /// Terrains are static triangle meshes in world space (rebuilt when they change).
    void syncTerrains() {
        auto& reg = scene.registry();
        for (auto it = terrains.begin(); it != terrains.end();) {
            const auto* t = reg.valid(it->first) ? reg.try_get<TerrainComponent>(it->first) : nullptr;
            if (t == nullptr || terrainBodyKey(*t, scene.worldMatrix(it->first), projectRoot) != it->second.key) {
                releaseConstraintsOf(it->first);
                removeTerrain(it->second);
                it = terrains.erase(it);
            } else {
                ++it;
            }
        }
        for (auto [e, terrain] : reg.view<TerrainComponent>().each()) {
            if (terrains.contains(e)) continue;
            const glm::mat4 world = scene.worldMatrix(e);
            const auto data = terrainData(terrain, projectRoot);
            std::vector<glm::vec3> vertices;
            std::vector<std::uint32_t> indices;
            terrainTriangles(*data, world, vertices, indices);
            JPH::VertexList joltVertices;
            joltVertices.reserve(vertices.size());
            for (const glm::vec3& v : vertices) joltVertices.push_back(JPH::Float3(v.x, v.y, v.z));
            JPH::IndexedTriangleList triangles;
            triangles.reserve(indices.size() / 3);
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
            JPH::MeshShapeSettings mesh(std::move(joltVertices), std::move(triangles));
            auto shape = mesh.Create();
            if (!shape.IsValid()) {
                log::error("Physics", "terrain '{}': {}", scene.nameOf(e), shape.GetError().c_str());
                continue;
            }
            JPH::BodyCreationSettings settings(shape.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                               Layers::kStatic);
            settings.mUserData = static_cast<JPH::uint64>(entt::to_integral(e));
            const JPH::BodyID id = system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
            if (id.IsInvalid()) continue;
            terrains.emplace(e, TerrainBody{id, terrainBodyKey(terrain, world, projectRoot)});
            system.OptimizeBroadPhase();
        }
    }

    /// Dynamic bodies inside a Water volume float: Jolt's buoyancy with drag.
    void applyBuoyancy(float dt) {
        auto& reg = scene.registry();
        auto waters = reg.view<WaterComponent>();
        if (waters.begin() == waters.end()) return;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        const JPH::Vec3 gravity = system.GetGravity();
        for (auto [w, water] : waters.each()) {
            if (water.buoyancy <= 0.0f) continue;
            const glm::mat4 world = scene.worldMatrix(w);
            const glm::mat4 inverse = glm::inverse(world);
            const glm::vec3 up = glm::normalize(glm::vec3(world[1]));
            for (const auto& [entity, body] : bodies) {
                if (body.motion != BodyMotion::Dynamic) continue;
                const glm::vec3 p = fromJolt(JPH::Vec3(bi.GetCenterOfMassPosition(body.id)));
                const glm::vec3 local(inverse * glm::vec4(p, 1.0f));
                if (std::abs(local.x) > water.width * 0.5f || std::abs(local.z) > water.length * 0.5f) continue;
                const glm::vec3 surface(world * glm::vec4(local.x, 0.0f, local.z, 1.0f));
                bi.ApplyBuoyancyImpulse(body.id, JPH::RVec3(toJolt(surface)), toJolt(up), water.buoyancy, 0.5f, 0.05f,
                                        JPH::Vec3::sZero(), gravity, dt);
            }
        }
    }

    explicit Impl(Scene& s) : scene(s) {}

    [[nodiscard]] entt::entity findNamed(const std::string& name) const {
        for (auto e : scene.registry().view<TransformComponent>()) {
            if (scene.nameOf(e) == name) return e;
        }
        return entt::null;
    }

    // ---- Joints ----------------------------------------------------------------------------

    void removeJoint(Joint& joint) {
        if (joint.constraint) system.RemoveConstraint(joint.constraint);
        joint.constraint = nullptr;
    }

    /// Removes joints, vehicles and ragdolls that hold on to an entity's body (before it goes).
    void releaseConstraintsOf(entt::entity e) {
        for (auto it = joints.begin(); it != joints.end();) {
            if (it->first == e || it->second.other == e) {
                removeJoint(it->second);
                it = joints.erase(it);
            } else {
                ++it;
            }
        }
        if (auto it = vehicles.find(e); it != vehicles.end()) {
            removeVehicle(it->second);
            vehicles.erase(it);
        }
    }

    void createJoint(entt::entity e, const JointComponent& jc) {
        const auto self = bodies.find(e);
        if (self == bodies.end()) return;
        Joint joint;
        joint.type = jc.type;
        joint.connectedTo = jc.connectedTo;
        JPH::BodyID otherId;
        if (!jc.connectedTo.empty()) {
            joint.other = findNamed(jc.connectedTo);
            const auto other = bodies.find(joint.other);
            if (joint.other == e || other == bodies.end()) {
                log::warn("Physics", "joint on '{}': no body named '{}' to connect to", scene.nameOf(e), jc.connectedTo);
                joints.emplace(e, std::move(joint));  // remembered, so the warning shows once
                return;
            }
            otherId = other->second.id;
        }
        const glm::mat4 world = scene.worldMatrix(e);
        const glm::vec3 pivot(world * glm::vec4(jc.anchor, 1.0f));
        const glm::vec3 axis = glm::length(jc.axis) > 1e-6f ? glm::normalize(glm::mat3(world) * jc.axis) : glm::vec3(0, 1, 0);
        const glm::vec3 normal = perpendicular(axis);
        JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
        switch (jc.type) {
            case JointType::Fixed: {
                auto* s = new JPH::FixedConstraintSettings;
                s->mAutoDetectPoint = true;
                settings = s;
                break;
            }
            case JointType::Hinge: {
                auto* s = new JPH::HingeConstraintSettings;
                s->mPoint1 = s->mPoint2 = JPH::RVec3(toJolt(pivot));
                s->mHingeAxis1 = s->mHingeAxis2 = toJolt(axis);
                s->mNormalAxis1 = s->mNormalAxis2 = toJolt(normal);
                if (jc.limits) {
                    s->mLimitsMin = glm::radians(std::clamp(jc.minLimit, -180.0f, 0.0f));
                    s->mLimitsMax = glm::radians(std::clamp(jc.maxLimit, 0.0f, 180.0f));
                }
                s->mMotorSettings.SetTorqueLimit(std::max(jc.motorForce, 0.0f));
                settings = s;
                break;
            }
            case JointType::Ball: {
                auto* s = new JPH::PointConstraintSettings;
                s->mPoint1 = s->mPoint2 = JPH::RVec3(toJolt(pivot));
                settings = s;
                break;
            }
            case JointType::Slider: {
                auto* s = new JPH::SliderConstraintSettings;
                s->mAutoDetectPoint = true;
                s->SetSliderAxis(toJolt(axis));
                if (jc.limits) {
                    s->mLimitsMin = std::min(jc.minLimit, 0.0f);
                    s->mLimitsMax = std::max(jc.maxLimit, 0.0f);
                }
                s->mMotorSettings.SetForceLimit(std::max(jc.motorForce, 0.0f));
                settings = s;
                break;
            }
            case JointType::Distance: {
                // With the world: a rope from the anchor (fixed where it is now) to the entity's
                // origin. With another body: between both origins.
                auto* s = new JPH::DistanceConstraintSettings;
                const glm::vec3 origin(world[3]);
                s->mPoint1 = JPH::RVec3(toJolt(joint.other == entt::null ? pivot : glm::vec3(scene.worldMatrix(joint.other)[3])));
                s->mPoint2 = JPH::RVec3(toJolt(origin));
                s->mMinDistance = std::max(jc.minDistance, 0.0f);
                s->mMaxDistance = jc.maxDistance > 0.0f ? std::max(jc.maxDistance, s->mMinDistance) : -1.0f;
                s->mLimitsSpringSettings.mFrequency = std::max(jc.spring, 0.0f);
                s->mLimitsSpringSettings.mDamping = 0.5f;
                settings = s;
                break;
            }
        }
        JPH::BodyInterface& bi = system.GetBodyInterface();
        joint.constraint = bi.CreateConstraint(settings, otherId, self->second.id);
        if (!joint.constraint) return;
        system.AddConstraint(joint.constraint);
        bi.ActivateBody(self->second.id);
        joints.emplace(e, std::move(joint));
    }

    /// Creates, rebuilds and removes joints to match the components; motors follow live.
    void syncJoints() {
        auto& reg = scene.registry();
        for (auto it = joints.begin(); it != joints.end();) {
            const auto* jc = reg.valid(it->first) ? reg.try_get<JointComponent>(it->first) : nullptr;
            if (jc == nullptr || jc->type != it->second.type || jc->connectedTo != it->second.connectedTo) {
                removeJoint(it->second);
                it = joints.erase(it);
            } else {
                ++it;
            }
        }
        for (auto [e, jc] : reg.view<JointComponent>().each()) {
            const auto it = joints.find(e);
            if (it == joints.end()) {
                createJoint(e, jc);
                continue;
            }
            JPH::TwoBodyConstraint* c = it->second.constraint;
            if (c == nullptr) continue;
            const bool motor = jc.motorForce > 0.0f;
            if (jc.type == JointType::Hinge) {
                auto* hinge = static_cast<JPH::HingeConstraint*>(c);
                hinge->GetMotorSettings().SetTorqueLimit(std::max(jc.motorForce, 0.0f));
                hinge->SetMotorState(motor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
                hinge->SetTargetAngularVelocity(glm::radians(jc.motorSpeed));
                if (jc.limits) {
                    hinge->SetLimits(glm::radians(std::clamp(jc.minLimit, -180.0f, 0.0f)), glm::radians(std::clamp(jc.maxLimit, 0.0f, 180.0f)));
                } else {
                    hinge->SetLimits(-JPH::JPH_PI, JPH::JPH_PI);
                }
            } else if (jc.type == JointType::Slider) {
                auto* slider = static_cast<JPH::SliderConstraint*>(c);
                slider->GetMotorSettings().SetForceLimit(std::max(jc.motorForce, 0.0f));
                slider->SetMotorState(motor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
                slider->SetTargetVelocity(jc.motorSpeed);
                if (jc.limits) {
                    slider->SetLimits(std::min(jc.minLimit, 0.0f), std::max(jc.maxLimit, 0.0f));
                } else {
                    slider->SetLimits(-FLT_MAX, FLT_MAX);
                }
            }
            if (motor) system.GetBodyInterface().ActivateBody(c->GetBody2()->GetID());
        }
    }

    // ---- Vehicles --------------------------------------------------------------------------

    void removeVehicle(Vehicle& v) {
        if (!v.constraint) return;
        system.RemoveStepListener(v.constraint);
        system.RemoveConstraint(v.constraint);
        v.constraint = nullptr;
    }

    void createVehicle(entt::entity e, const VehicleComponent& vc) {
        const auto self = bodies.find(e);
        auto& reg = scene.registry();
        const auto* collider = reg.try_get<ColliderComponent>(e);
        if (self == bodies.end() || self->second.motion != BodyMotion::Dynamic || collider == nullptr) {
            vehicles.emplace(e, Vehicle{});  // needs a dynamic body; remembered so it is not retried every step
            log::warn("Physics", "vehicle '{}' needs a Collider and a dynamic Rigid Body", scene.nameOf(e));
            return;
        }
        const Pose pose = decompose(scene.worldMatrix(e));
        const glm::vec3 half = (collider->shape == ColliderShape::Box ? collider->size * 0.5f : glm::vec3(collider->radius)) *
                               glm::abs(pose.scale);
        const glm::vec3 centre = collider->offset * glm::abs(pose.scale);
        const float radius = std::max(vc.wheelRadius, 0.05f);
        const float width = std::max(vc.wheelWidth, 0.02f);
        JPH::VehicleConstraintSettings settings;
        settings.mUp = JPH::Vec3(0, 1, 0);
        settings.mForward = JPH::Vec3(0, 0, -1);  // entities face -Z
        settings.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);
        // Front left, front right, rear left, rear right.
        const float x = std::max(half.x - width * 0.5f, 0.05f);
        const float z = std::max(half.z - radius, 0.05f);
        const glm::vec3 corners[4] = {{-x, 0, -z}, {x, 0, -z}, {-x, 0, z}, {x, 0, z}};
        for (int i = 0; i < 4; ++i) {
            auto* wheel = new JPH::WheelSettingsWV;
            const glm::vec3 p = centre + corners[i] + glm::vec3(0.0f, -half.y * 0.5f, 0.0f);
            wheel->mPosition = toJolt(p);
            wheel->mWheelForward = JPH::Vec3(0, 0, -1);
            wheel->mRadius = radius;
            wheel->mWidth = width;
            wheel->mSuspensionMinLength = std::max(vc.suspensionLength * 0.2f, 0.02f);
            wheel->mSuspensionMaxLength = std::max(vc.suspensionLength, wheel->mSuspensionMinLength + 0.01f);
            wheel->mMaxSteerAngle = i < 2 ? JPH::DegreesToRadians(std::clamp(vc.maxSteerDeg, 0.0f, 80.0f)) : 0.0f;
            wheel->mMaxHandBrakeTorque = i < 2 ? 0.0f : 4000.0f;
            settings.mWheels.push_back(wheel);
        }
        auto* controller = new JPH::WheeledVehicleControllerSettings;
        controller->mEngine.mMaxTorque = std::max(vc.engineTorque, 0.0f);
        controller->mDifferentials.resize(2);  // four-wheel drive
        controller->mDifferentials[0].mLeftWheel = 0;
        controller->mDifferentials[0].mRightWheel = 1;
        controller->mDifferentials[0].mEngineTorqueRatio = 0.5f;
        controller->mDifferentials[1].mLeftWheel = 2;
        controller->mDifferentials[1].mRightWheel = 3;
        controller->mDifferentials[1].mEngineTorqueRatio = 0.5f;
        settings.mController = controller;

        JPH::BodyLockWrite lock(system.GetBodyLockInterface(), self->second.id);
        if (!lock.Succeeded()) return;
        Vehicle v;
        v.constraint = new JPH::VehicleConstraint(lock.GetBody(), settings);
        v.tester = new JPH::VehicleCollisionTesterCastCylinder(Layers::kMoving);
        v.constraint->SetVehicleCollisionTester(v.tester);
        static constexpr const char* kWheelNames[4] = {"Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR"};
        for (entt::entity child : scene.childrenOf(e)) {
            for (int i = 0; i < 4; ++i) {
                if (scene.nameOf(child) == kWheelNames[i]) v.wheels[static_cast<std::size_t>(i)] = child;
            }
        }
        system.AddConstraint(v.constraint);
        system.AddStepListener(v.constraint);
        vehicles.emplace(e, std::move(v));
    }

    void syncVehicles() {
        auto& reg = scene.registry();
        for (auto it = vehicles.begin(); it != vehicles.end();) {
            if (!reg.valid(it->first) || !reg.all_of<VehicleComponent>(it->first)) {
                removeVehicle(it->second);
                it = vehicles.erase(it);
            } else {
                ++it;
            }
        }
        for (auto [e, vc] : reg.view<VehicleComponent>().each()) {
            if (!vehicles.contains(e)) createVehicle(e, vc);
        }
    }

    // ---- Ragdolls --------------------------------------------------------------------------

    void removeRagdoll(RagdollInstance& r) {
        if (r.ragdoll) r.ragdoll->RemoveFromPhysicsSystem();
        r.ragdoll = nullptr;  // destroys its bodies and constraints
    }

    void createRagdoll(entt::entity root, const RagdollComponent& rc) {
        auto& reg = scene.registry();
        RagdollInstance r;
        std::vector<int> parents;
        std::unordered_map<entt::entity, int> index;
        // Bones: the model's nodes that carry no mesh, parents first.
        std::function<void(entt::entity, int)> collect = [&](entt::entity e, int parent) {
            int self = parent;
            if (e != root && reg.all_of<ModelNodeComponent>(e) && !reg.any_of<MeshRendererComponent, SkinComponent>(e)) {
                self = static_cast<int>(r.bones.size());
                index[e] = self;
                r.bones.push_back(e);
                parents.push_back(parent);
            }
            for (entt::entity child : scene.childrenOf(e)) collect(child, self);
        };
        collect(root, -1);
        if (r.bones.empty()) {
            log::warn("Physics", "ragdoll '{}' has no bones (needs an imported, skinned model)", scene.nameOf(root));
            ragdolls.emplace(root, std::move(r));
            return;
        }
        const std::size_t n = r.bones.size();
        std::vector<Pose> poses(n);
        for (std::size_t i = 0; i < n; ++i) poses[i] = decompose(scene.worldMatrix(r.bones[i]));
        // Each bone's direction: toward the average of its children, else continuing its parent.
        std::vector<glm::vec3> tips(n);
        std::vector<int> childCount(n, 0);
        for (std::size_t i = 0; i < n; ++i) tips[i] = glm::vec3(0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            if (parents[i] >= 0) {
                tips[static_cast<std::size_t>(parents[i])] += poses[i].position;
                ++childCount[static_cast<std::size_t>(parents[i])];
            }
        }
        JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton;
        JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;
        settings->mSkeleton = skeleton;
        settings->mParts.resize(n);
        const float massPerBone = std::max(rc.mass, 0.1f) / static_cast<float>(n);
        std::vector<float> lengths(n, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            skeleton->AddJoint(scene.nameOf(r.bones[i]), parents[i]);
            const glm::vec3 pos = poses[i].position;
            glm::vec3 dir(0.0f, 1.0f, 0.0f);
            float length = 0.0f;
            if (childCount[i] > 0) {
                const glm::vec3 tip = tips[i] / static_cast<float>(childCount[i]);
                length = glm::length(tip - pos);
                if (length > 1e-4f) dir = (tip - pos) / length;
            } else if (parents[i] >= 0) {
                const glm::vec3 from = poses[static_cast<std::size_t>(parents[i])].position;
                if (glm::length(pos - from) > 1e-4f) dir = glm::normalize(pos - from);
                length = lengths[static_cast<std::size_t>(parents[i])] * 0.5f;
            }
            lengths[i] = length;
            const float radius = std::clamp(length * 0.25f, 0.02f, 0.15f);
            // The body sits at the bone with the bone's rotation; the shape reaches toward the tip.
            const glm::vec3 localDir = glm::inverse(poses[i].rotation) * dir;
            JPH::RefConst<JPH::Shape> shape;
            const float halfCylinder = 0.5f * length - radius;
            if (halfCylinder > 1e-3f) {
                shape = new JPH::CapsuleShape(halfCylinder, radius);
            } else {
                shape = new JPH::SphereShape(radius);
            }
            JPH::RotatedTranslatedShapeSettings placed(toJolt(localDir * (0.5f * length)),
                                                       toJolt(glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), localDir)), shape);
            auto placedShape = placed.Create();
            JPH::RagdollSettings::Part& part = settings->mParts[i];
            part.SetShape(placedShape.IsValid() ? placedShape.Get() : shape);
            part.mPosition = JPH::RVec3(toJolt(pos));
            part.mRotation = toJolt(poses[i].rotation);
            part.mMotionType = JPH::EMotionType::Dynamic;
            part.mObjectLayer = Layers::kMoving;
            part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = massPerBone;
            part.mLinearDamping = 0.1f;
            part.mAngularDamping = 0.6f;
            if (parents[i] >= 0) {
                auto* joint = new JPH::SwingTwistConstraintSettings;
                joint->mSpace = JPH::EConstraintSpace::WorldSpace;
                joint->mPosition1 = joint->mPosition2 = JPH::RVec3(toJolt(pos));
                joint->mTwistAxis1 = joint->mTwistAxis2 = toJolt(dir);
                joint->mPlaneAxis1 = joint->mPlaneAxis2 = toJolt(perpendicular(dir));
                joint->mNormalHalfConeAngle = JPH::DegreesToRadians(40.0f);
                joint->mPlaneHalfConeAngle = JPH::DegreesToRadians(40.0f);
                joint->mTwistMinAngle = JPH::DegreesToRadians(-30.0f);
                joint->mTwistMaxAngle = JPH::DegreesToRadians(30.0f);
                part.mToParent = joint;
            }
            r.scales.push_back(poses[i].scale);
        }
        skeleton->CalculateParentJointIndices();
        settings->Stabilize();
        settings->DisableParentChildCollisions();
        settings->CalculateBodyIndexToConstraintIndex();
        settings->CalculateConstraintIndexToBodyIdxPair();
        r.ragdoll = settings->CreateRagdoll(nextRagdollGroup++, static_cast<JPH::uint64>(entt::to_integral(root)), &system);
        if (!r.ragdoll) {
            log::error("Physics", "could not create the ragdoll of '{}' (body limit reached?)", scene.nameOf(root));
            r.bones.clear();
            ragdolls.emplace(root, std::move(r));
            return;
        }
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (std::size_t i = 0; i < n; ++i) {
            bi.SetUserData(r.ragdoll->GetBodyID(static_cast<int>(i)), static_cast<JPH::uint64>(entt::to_integral(r.bones[i])));
        }
        r.ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
        ragdolls.emplace(root, std::move(r));
    }

    void syncRagdolls() {
        auto& reg = scene.registry();
        for (auto it = ragdolls.begin(); it != ragdolls.end();) {
            const auto* rc = reg.valid(it->first) ? reg.try_get<RagdollComponent>(it->first) : nullptr;
            const bool bonesAlive = std::ranges::all_of(it->second.bones, [&](entt::entity b) { return reg.valid(b); });
            if (rc == nullptr || !rc->active || !bonesAlive) {
                removeRagdoll(it->second);
                it = ragdolls.erase(it);
            } else {
                ++it;
            }
        }
        for (auto [e, rc] : reg.view<RagdollComponent>().each()) {
            if (rc.active && !ragdolls.contains(e)) createRagdoll(e, rc);
        }
    }

    void createCharacter(entt::entity e) {
        const auto& cc = scene.registry().get<CharacterControllerComponent>(e);
        const float radius = std::max(cc.radius, 0.05f);
        const float height = std::max(cc.height, 2.0f * radius + 0.01f);
        const float halfCylinder = 0.5f * (height - 2.0f * radius);
        // The capsule stands on the entity's origin: its centre is half the height up.
        JPH::RotatedTranslatedShapeSettings standing(JPH::Vec3(0.0f, 0.5f * height, 0.0f), JPH::Quat::sIdentity(),
                                                     new JPH::CapsuleShape(halfCylinder, radius));
        auto shape = standing.Create();
        if (!shape.IsValid()) return;
        JPH::CharacterVirtualSettings settings;
        settings.mShape = shape.Get();
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(std::clamp(cc.maxSlopeDeg, 0.0f, 89.0f));
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);  // only the bottom sphere stands
        const Pose pose = decompose(scene.worldMatrix(e));
        Character c;
        c.character = new JPH::CharacterVirtual(&settings, JPH::RVec3(toJolt(pose.position)), JPH::Quat::sIdentity(),
                                                static_cast<JPH::uint64>(entt::to_integral(e)), &system);
        c.character->SetListener(&characterContacts);
        c.gravityFactor = cc.gravityFactor;
        c.stepHeight = cc.stepHeight;
        c.lastWritten = pose.position;
        characters.emplace(e, std::move(c));
    }

    void syncCharacters() {
        auto& reg = scene.registry();
        std::erase_if(characters, [&](const auto& entry) {
            return !reg.valid(entry.first) || !reg.all_of<CharacterControllerComponent>(entry.first);
        });
        for (auto e : reg.view<CharacterControllerComponent>()) {
            if (!characters.contains(e)) createCharacter(e);
        }
    }

    void createBody(entt::entity e) {
        const auto& reg = scene.registry();
        const auto& collider = reg.get<ColliderComponent>(e);
        const auto* rb = reg.try_get<RigidBodyComponent>(e);
        const BodyMotion motion = rb != nullptr ? rb->motion : BodyMotion::Static;
        const Pose pose = decompose(scene.worldMatrix(e));
        const JPH::EMotionType type = motion == BodyMotion::Dynamic   ? JPH::EMotionType::Dynamic
                                      : motion == BodyMotion::Kinematic ? JPH::EMotionType::Kinematic
                                                                        : JPH::EMotionType::Static;
        JPH::BodyCreationSettings settings(makeShape(collider, pose.scale), JPH::RVec3(toJolt(pose.position)),
                                           toJolt(pose.rotation), type,
                                           motion == BodyMotion::Static ? Layers::kStatic : Layers::kMoving);
        settings.mUserData = static_cast<JPH::uint64>(entt::to_integral(e));  // contacts and raycasts name the entity
        settings.mIsSensor = collider.isTrigger;  // reports contacts, blocks nothing
        settings.mCollideKinematicVsNonDynamic = collider.isTrigger;  // kinematic triggers see static/kinematic too
        if (rb != nullptr) {
            settings.mFriction = rb->friction;
            settings.mRestitution = rb->restitution;
            settings.mLinearDamping = rb->linearDamping;
            settings.mAngularDamping = rb->angularDamping;
            settings.mGravityFactor = rb->gravityFactor;
            if (motion == BodyMotion::Dynamic) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = std::max(rb->mass, 1e-3f);
            }
        }
        JPH::BodyInterface& bodiesInterface = system.GetBodyInterface();
        const JPH::BodyID id = bodiesInterface.CreateAndAddBody(
            settings, motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (id.IsInvalid()) {
            log::error("Physics", "could not create a body for '{}' (body limit reached?)", scene.nameOf(e));
            return;
        }
        bodies.emplace(e, Body{id, motion});
    }

    void removeBody(const Body& body) {
        JPH::BodyInterface& bi = system.GetBodyInterface();
        bi.RemoveBody(body.id);
        bi.DestroyBody(body.id);
    }

    /// Adds bodies for new colliders and removes bodies whose entity or collider disappeared.
    void syncBodies() {
        auto& reg = scene.registry();
        for (auto it = bodies.begin(); it != bodies.end();) {
            if (!reg.valid(it->first) || !reg.all_of<ColliderComponent>(it->first)) {
                releaseConstraintsOf(it->first);
                removeBody(it->second);
                it = bodies.erase(it);
            } else {
                ++it;
            }
        }
        bool added = false;
        for (auto e : reg.view<ColliderComponent>()) {
            if (!bodies.contains(e)) {
                createBody(e);
                added = true;
            }
        }
        if (added) system.OptimizeBroadPhase();
        syncTerrains();
        syncJoints();
        syncVehicles();
        syncRagdolls();
    }
};

PhysicsWorld::PhysicsWorld(Scene& scene) : impl_(std::make_unique<Impl>(scene)) {
    acquireJolt();
    constexpr JPH::uint kMaxBodies = 65536;
    constexpr JPH::uint kMaxBodyPairs = 65536;
    constexpr JPH::uint kMaxContacts = 20480;
    impl_->temp = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    const unsigned hw = std::thread::hardware_concurrency();
    const int threads = static_cast<int>(std::clamp(hw > 1 ? hw - 1 : 1u, 1u, 4u));
    impl_->jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
    impl_->system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, impl_->broadPhase, impl_->objectVsBroadPhase,
                       impl_->objectPairs);
    impl_->system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    impl_->system.SetContactListener(&impl_->contacts);
    scene.updateWorldTransforms();
    impl_->syncBodies();
    impl_->syncCharacters();
    log::info("Physics", "world created with {} bodies, {} characters ({} worker threads)", impl_->bodies.size(),
              impl_->characters.size(), threads);
}

PhysicsWorld::~PhysicsWorld() {
    impl_->characters.clear();
    for (auto& [entity, joint] : impl_->joints) impl_->removeJoint(joint);
    impl_->joints.clear();
    for (auto& [entity, vehicle] : impl_->vehicles) impl_->removeVehicle(vehicle);
    impl_->vehicles.clear();
    for (auto& [entity, ragdoll] : impl_->ragdolls) impl_->removeRagdoll(ragdoll);
    impl_->ragdolls.clear();
    for (const auto& [entity, terrain] : impl_->terrains) impl_->removeTerrain(terrain);
    impl_->terrains.clear();
    for (const auto& [entity, body] : impl_->bodies) impl_->removeBody(body);
    impl_->bodies.clear();
    impl_.reset();
    releaseJolt();
}

bool PhysicsWorld::sceneHasColliders(const Scene& scene) {
    return !scene.registry().view<ColliderComponent>().empty() ||
           !scene.registry().view<CharacterControllerComponent>().empty() ||
           !scene.registry().view<TerrainComponent>().empty();
}

bool PhysicsWorld::moveCharacter(entt::entity entity, const glm::vec3& velocity) {
    const auto it = impl_->characters.find(entity);
    if (it == impl_->characters.end()) return false;
    it->second.desired = glm::vec3(velocity.x, 0.0f, velocity.z);
    return true;
}

bool PhysicsWorld::jumpCharacter(entt::entity entity, float speed) {
    const auto it = impl_->characters.find(entity);
    if (it == impl_->characters.end() || !characterGrounded(entity)) return false;
    it->second.pendingJump = speed;
    return true;
}

bool PhysicsWorld::characterGrounded(entt::entity entity) const {
    const auto it = impl_->characters.find(entity);
    return it != impl_->characters.end() &&
           it->second.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

std::size_t PhysicsWorld::characterCount() const { return impl_->characters.size(); }

std::size_t PhysicsWorld::bodyCount() const { return impl_->bodies.size(); }

void PhysicsWorld::setProjectRoot(std::filesystem::path root) {
    impl_->projectRoot = std::move(root);
    impl_->syncTerrains();  // heightmaps can be read now
}

std::size_t PhysicsWorld::terrainCount() const { return impl_->terrains.size(); }

std::size_t PhysicsWorld::jointCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->joints, [](const auto& j) { return j.second.constraint != nullptr; }));
}

std::size_t PhysicsWorld::vehicleCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->vehicles, [](const auto& v) { return v.second.constraint != nullptr; }));
}

std::size_t PhysicsWorld::ragdollBodyCount() const {
    std::size_t count = 0;
    for (const auto& [entity, r] : impl_->ragdolls) count += r.ragdoll ? r.ragdoll->GetBodyCount() : 0;
    return count;
}

bool PhysicsWorld::driveVehicle(entt::entity entity, float throttle, float steering, float brake, float handbrake) {
    const auto it = impl_->vehicles.find(entity);
    if (it == impl_->vehicles.end() || !it->second.constraint) return false;
    auto* controller = static_cast<JPH::WheeledVehicleController*>(it->second.constraint->GetController());
    controller->SetDriverInput(std::clamp(throttle, -1.0f, 1.0f), std::clamp(steering, -1.0f, 1.0f),
                               std::clamp(brake, 0.0f, 1.0f), std::clamp(handbrake, 0.0f, 1.0f));
    if (throttle != 0.0f || steering != 0.0f) impl_->system.GetBodyInterface().ActivateBody(impl_->bodies.at(entity).id);
    return true;
}

float PhysicsWorld::vehicleSpeed(entt::entity entity) const {
    const auto it = impl_->vehicles.find(entity);
    if (it == impl_->vehicles.end() || !it->second.constraint) return 0.0f;
    const JPH::Body* body = it->second.constraint->GetVehicleBody();
    const JPH::Vec3 forward = body->GetRotation() * it->second.constraint->GetLocalForward();
    return body->GetLinearVelocity().Dot(forward);
}

void PhysicsWorld::setGravity(const glm::vec3& gravity) { impl_->system.SetGravity(toJolt(gravity)); }

glm::vec3 PhysicsWorld::linearVelocity(entt::entity entity) const {
    const auto it = impl_->bodies.find(entity);
    if (it == impl_->bodies.end()) return glm::vec3(0.0f);
    return fromJolt(impl_->system.GetBodyInterface().GetLinearVelocity(it->second.id));
}

bool PhysicsWorld::setLinearVelocity(entt::entity entity, const glm::vec3& velocity) {
    const auto it = impl_->bodies.find(entity);
    if (it == impl_->bodies.end() || it->second.motion != BodyMotion::Dynamic) return false;
    impl_->system.GetBodyInterface().SetLinearVelocity(it->second.id, toJolt(velocity));
    return true;
}

bool PhysicsWorld::addForce(entt::entity entity, const glm::vec3& force, bool impulse) {
    const auto it = impl_->bodies.find(entity);
    if (it == impl_->bodies.end() || it->second.motion != BodyMotion::Dynamic) return false;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    if (impulse) {
        bi.AddImpulse(it->second.id, toJolt(force));
    } else {
        bi.AddForce(it->second.id, toJolt(force));
    }
    bi.ActivateBody(it->second.id);
    return true;
}

std::optional<PhysicsWorld::RayHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                                          float maxDistance) const {
    const float length = glm::length(direction);
    if (length < 1e-6f || maxDistance <= 0.0f) return std::nullopt;
    const glm::vec3 dir = direction / length;
    const JPH::RRayCast ray{JPH::RVec3(toJolt(origin)), toJolt(dir * maxDistance)};
    JPH::RayCastResult result;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) return std::nullopt;
    RayHit hit;
    hit.distance = result.mFraction * maxDistance;
    hit.point = origin + dir * hit.distance;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.entity = static_cast<entt::entity>(static_cast<std::uint32_t>(body.GetUserData()));
        hit.normal = fromJolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
    }
    return hit;
}

std::vector<std::pair<entt::entity, entt::entity>> PhysicsWorld::takeContacts() {
    std::scoped_lock lock(impl_->contacts.mutex);
    return std::exchange(impl_->contacts.pairs, {});
}

void PhysicsWorld::refreshBody(entt::entity entity) {
    const auto it = impl_->bodies.find(entity);
    if (it == impl_->bodies.end()) return;
    impl_->releaseConstraintsOf(entity);
    impl_->removeBody(it->second);
    impl_->bodies.erase(it);
    if (impl_->scene.registry().valid(entity) && impl_->scene.registry().all_of<ColliderComponent>(entity)) {
        impl_->scene.updateWorldTransforms();
        impl_->createBody(entity);
    }
}

void PhysicsWorld::step(float dt) {
    Scene& scene = impl_->scene;
    impl_->syncBodies();
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();

    // Kinematic bodies follow their entity (which gameplay systems may have moved).
    for (const auto& [entity, body] : impl_->bodies) {
        if (body.motion != BodyMotion::Kinematic) continue;
        const Pose pose = decompose(scene.worldMatrix(entity));
        bi.MoveKinematic(body.id, JPH::RVec3(toJolt(pose.position)), toJolt(pose.rotation), dt);
    }

    impl_->applyBuoyancy(dt);
    const JPH::EPhysicsUpdateError error = impl_->system.Update(dt, 1, impl_->temp.get(), impl_->jobs.get());
    if (error != JPH::EPhysicsUpdateError::None) {
        log::warn("Physics", "update reported limits being exceeded (error {})", static_cast<int>(error));
    }

    // Characters: walk at their desired velocity, fall, jump, slide along what they hit.
    impl_->syncCharacters();
    auto& reg = scene.registry();
    const JPH::Vec3 gravity = impl_->system.GetGravity();
    for (auto& [entity, c] : impl_->characters) {
        // Moved by something else (a script, the editor): teleport the character there.
        const glm::vec3 scenePos(scene.worldMatrix(entity)[3]);
        if (glm::length(scenePos - c.lastWritten) > 1e-4f) {
            c.character->SetPosition(JPH::RVec3(toJolt(scenePos)));
            c.vertical = 0.0f;
        }
        const bool grounded = c.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        if (grounded && c.vertical <= 0.0f) c.vertical = 0.0f;
        if (c.pendingJump > 0.0f) {
            c.vertical = c.pendingJump;
            c.pendingJump = 0.0f;
        } else if (!grounded) {
            c.vertical += gravity.GetY() * c.gravityFactor * dt;
        }
        // Standing on something that moves (a platform): move with it.
        c.character->UpdateGroundVelocity();
        const glm::vec3 ground = grounded ? fromJolt(c.character->GetGroundVelocity()) : glm::vec3(0.0f);
        c.character->SetLinearVelocity(JPH::Vec3(c.desired.x + ground.x, c.vertical + ground.y, c.desired.z + ground.z));
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, c.stepHeight, 0.0f);
        c.character->ExtendedUpdate(dt, gravity * c.gravityFactor, update,
                                    impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
                                    impl_->system.GetDefaultLayerFilter(Layers::kMoving), {}, {}, *impl_->temp);
        // Landing on something stops the fall; hitting a ceiling stops the rise.
        const glm::vec3 moved = fromJolt(c.character->GetLinearVelocity()) - ground;
        if (c.vertical > 0.0f && moved.y < c.vertical * 0.5f) c.vertical = std::max(moved.y, 0.0f);
        glm::vec3 worldPos = fromJolt(JPH::Vec3(c.character->GetPosition()));
        c.lastWritten = worldPos;
        const entt::entity parent = scene.parentOf(entity);
        if (parent != entt::null) worldPos = glm::vec3(glm::inverse(scene.worldMatrix(parent)) * glm::vec4(worldPos, 1.0f));
        reg.get<TransformComponent>(entity).position = worldPos;
    }

    // Dynamic bodies write their pose back, converted into parent space.
    for (const auto& [entity, body] : impl_->bodies) {
        if (body.motion != BodyMotion::Dynamic || !bi.IsActive(body.id)) continue;
        JPH::RVec3 position;
        JPH::Quat rotation;
        bi.GetPositionAndRotation(body.id, position, rotation);
        const entt::entity parent = scene.parentOf(entity);
        const glm::mat4 parentWorld = parent != entt::null ? scene.worldMatrix(parent) : glm::mat4(1.0f);
        writeWorldPose(scene, entity, fromJolt(JPH::Vec3(position)), fromJolt(rotation),
                       parent != entt::null ? &parentWorld : nullptr);
    }

    // Wheels named "Wheel FL/FR/RL/RR" under a vehicle follow its wheels (cylinders stand on Y,
    // so the wheel's axle is the model's Y axis).
    for (const auto& [entity, v] : impl_->vehicles) {
        if (!v.constraint) continue;
        const glm::mat4 parentWorld = [&] {
            JPH::RVec3 position;
            JPH::Quat rotation;
            bi.GetPositionAndRotation(v.constraint->GetVehicleBody()->GetID(), position, rotation);
            const glm::vec3 scale = decompose(scene.worldMatrix(entity)).scale;
            return glm::translate(glm::mat4(1.0f), fromJolt(JPH::Vec3(position))) * glm::mat4_cast(fromJolt(rotation)) *
                   glm::scale(glm::mat4(1.0f), scale);
        }();
        for (std::uint32_t i = 0; i < 4; ++i) {
            const entt::entity wheel = v.wheels[i];
            if (wheel == entt::null || !reg.valid(wheel)) continue;
            const JPH::RMat44 m = v.constraint->GetWheelWorldTransform(i, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
            writeWorldPose(scene, wheel, fromJolt(JPH::Vec3(m.GetTranslation())), fromJolt(m.GetQuaternion()), &parentWorld);
        }
    }

    // Ragdoll bones: their bodies' poses, parents first (children use the parent's new pose).
    for (const auto& [root, r] : impl_->ragdolls) {
        if (!r.ragdoll) continue;
        std::unordered_map<entt::entity, glm::mat4> updated;
        for (std::size_t i = 0; i < r.bones.size(); ++i) {
            JPH::RVec3 position;
            JPH::Quat rotation;
            bi.GetPositionAndRotation(r.ragdoll->GetBodyID(static_cast<int>(i)), position, rotation);
            const glm::vec3 pos = fromJolt(JPH::Vec3(position));
            const glm::quat rot = fromJolt(rotation);
            const entt::entity parent = scene.parentOf(r.bones[i]);
            glm::mat4 parentWorld(1.0f);
            if (parent != entt::null) {
                const auto found = updated.find(parent);
                parentWorld = found != updated.end() ? found->second : scene.worldMatrix(parent);
            }
            writeWorldPose(scene, r.bones[i], pos, rot, parent != entt::null ? &parentWorld : nullptr);
            updated[r.bones[i]] = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), r.scales[i]);
        }
    }
}

} // namespace lumora
