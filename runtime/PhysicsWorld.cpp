#include "runtime/PhysicsWorld.hpp"

#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/matrix.hpp>

// Jolt must be included with its configuration macros (exported by the Jolt target).
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
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

struct PhysicsWorld::Impl {
    Scene& scene;
    BroadPhaseLayerMapping broadPhase;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectPairFilter objectPairs;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    JPH::PhysicsSystem system;
    struct Body {
        JPH::BodyID id;
        BodyMotion motion;
    };
    std::unordered_map<entt::entity, Body> bodies;

    explicit Impl(Scene& s) : scene(s) {}

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
    scene.updateWorldTransforms();
    impl_->syncBodies();
    log::info("Physics", "world created with {} bodies ({} worker threads)", impl_->bodies.size(), threads);
}

PhysicsWorld::~PhysicsWorld() {
    for (const auto& [entity, body] : impl_->bodies) impl_->removeBody(body);
    impl_->bodies.clear();
    impl_.reset();
    releaseJolt();
}

bool PhysicsWorld::sceneHasColliders(const Scene& scene) {
    return !scene.registry().view<ColliderComponent>().empty();
}

std::size_t PhysicsWorld::bodyCount() const { return impl_->bodies.size(); }

void PhysicsWorld::setGravity(const glm::vec3& gravity) { impl_->system.SetGravity(toJolt(gravity)); }

glm::vec3 PhysicsWorld::linearVelocity(entt::entity entity) const {
    const auto it = impl_->bodies.find(entity);
    if (it == impl_->bodies.end()) return glm::vec3(0.0f);
    return fromJolt(impl_->system.GetBodyInterface().GetLinearVelocity(it->second.id));
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

    const JPH::EPhysicsUpdateError error = impl_->system.Update(dt, 1, impl_->temp.get(), impl_->jobs.get());
    if (error != JPH::EPhysicsUpdateError::None) {
        log::warn("Physics", "update reported limits being exceeded (error {})", static_cast<int>(error));
    }

    // Dynamic bodies write their pose back, converted into parent space.
    auto& reg = scene.registry();
    for (const auto& [entity, body] : impl_->bodies) {
        if (body.motion != BodyMotion::Dynamic || !bi.IsActive(body.id)) continue;
        JPH::RVec3 position;
        JPH::Quat rotation;
        bi.GetPositionAndRotation(body.id, position, rotation);
        glm::vec3 worldPos = fromJolt(JPH::Vec3(position));
        glm::quat worldRot = fromJolt(rotation);
        const entt::entity parent = scene.parentOf(entity);
        if (parent != entt::null) {
            const glm::mat4 parentWorld = scene.worldMatrix(parent);
            const Pose parentPose = decompose(parentWorld);
            worldPos = glm::vec3(glm::inverse(parentWorld) * glm::vec4(worldPos, 1.0f));
            worldRot = glm::inverse(parentPose.rotation) * worldRot;
        }
        auto& t = reg.get<TransformComponent>(entity);
        t.position = worldPos;
        float y = 0.0f, x = 0.0f, z = 0.0f;
        glm::extractEulerAngleYXZ(glm::mat4_cast(glm::normalize(worldRot)), y, x, z);
        t.rotationDeg = glm::degrees(glm::vec3(x, y, z));
    }
}

} // namespace lumora
