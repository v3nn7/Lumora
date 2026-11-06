#include "editor/EntityFactory.hpp"

#include "common/Theme.hpp"
#include "scene/Scene.hpp"

#include <QMenu>

namespace lumora::editor {

namespace {

EditorContext::EntityInit meshInit(MeshPrimitive primitive, float y) {
    return [primitive, y](Scene& scene, entt::entity e) {
        auto& mesh = scene.registry().emplace<MeshRendererComponent>(e);
        mesh.mesh = primitive;
        scene.registry().get<TransformComponent>(e).position.y = y;
    };
}

} // namespace

void populateCreateMenu(QMenu& menu, EditorContext& ctx, bool asChild) {
    using app::theme::icon;
    menu.addAction(icon(QStringLiteral("entity")), QObject::tr("Empty Entity"),
                   [&ctx, asChild] { ctx.createEntity(QObject::tr("Entity"), {}, asChild); });
    menu.addSeparator();
    menu.addAction(icon(QStringLiteral("cube")), QObject::tr("Cube"),
                   [&ctx, asChild] { ctx.createEntity(QObject::tr("Cube"), meshInit(MeshPrimitive::Cube, 0.5f), asChild); });
    menu.addAction(icon(QStringLiteral("sphere")), QObject::tr("Sphere"),
                   [&ctx, asChild] { ctx.createEntity(QObject::tr("Sphere"), meshInit(MeshPrimitive::Sphere, 0.5f), asChild); });
    menu.addAction(icon(QStringLiteral("cylinder")), QObject::tr("Cylinder"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Cylinder"), meshInit(MeshPrimitive::Cylinder, 0.5f), asChild);
    });
    menu.addAction(icon(QStringLiteral("plane")), QObject::tr("Plane"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Plane"), [](Scene& scene, entt::entity e) {
            scene.registry().emplace<MeshRendererComponent>(e).mesh = MeshPrimitive::Plane;
            scene.registry().get<TransformComponent>(e).scale = {10.0f, 1.0f, 10.0f};
        }, asChild);
    });
    QMenu* physics = menu.addMenu(icon(QStringLiteral("sparkle")), QObject::tr("Physics"));
    auto physicsBody = [](MeshPrimitive primitive, ColliderShape shape, float y) {
        return [primitive, shape, y](Scene& scene, entt::entity e) {
            auto& mesh = scene.registry().emplace<MeshRendererComponent>(e);
            mesh.mesh = primitive;
            mesh.baseColor = shape == ColliderShape::Sphere ? glm::vec3(0.9f, 0.55f, 0.15f) : glm::vec3(0.25f, 0.55f, 0.9f);
            auto& collider = scene.registry().emplace<ColliderComponent>(e);
            collider.shape = shape;
            scene.registry().emplace<RigidBodyComponent>(e);
            scene.registry().get<TransformComponent>(e).position.y = y;
        };
    };
    physics->addAction(icon(QStringLiteral("cube")), QObject::tr("Dynamic Cube"), [&ctx, asChild, physicsBody] {
        ctx.createEntity(QObject::tr("Physics Cube"), physicsBody(MeshPrimitive::Cube, ColliderShape::Box, 3.0f), asChild);
    });
    physics->addAction(icon(QStringLiteral("sphere")), QObject::tr("Dynamic Sphere"), [&ctx, asChild, physicsBody] {
        ctx.createEntity(QObject::tr("Physics Sphere"), physicsBody(MeshPrimitive::Sphere, ColliderShape::Sphere, 3.0f), asChild);
    });
    physics->addAction(icon(QStringLiteral("plane")), QObject::tr("Static Floor"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Floor"), [](Scene& scene, entt::entity e) {
            auto& mesh = scene.registry().emplace<MeshRendererComponent>(e);
            mesh.mesh = MeshPrimitive::Plane;
            mesh.pattern = SurfacePattern::Grid;
            auto& collider = scene.registry().emplace<ColliderComponent>(e);
            collider.size = {1.0f, 0.02f, 1.0f};
            collider.offset = {0.0f, -0.01f, 0.0f};
            scene.registry().get<TransformComponent>(e).scale = {20.0f, 1.0f, 20.0f};
        }, asChild);
    });
    physics->addAction(icon(QStringLiteral("cube")), QObject::tr("Moving Platform"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Moving Platform"), [](Scene& scene, entt::entity e) {
            auto& reg = scene.registry();
            auto& mesh = reg.emplace<MeshRendererComponent>(e);
            mesh.baseColor = {0.55f, 0.6f, 0.65f};
            mesh.pattern = SurfacePattern::Grid;
            reg.emplace<ColliderComponent>(e);
            reg.emplace<RigidBodyComponent>(e).motion = BodyMotion::Kinematic;
            reg.emplace<OscillatorComponent>(e).offset = {3.0f, 0.0f, 0.0f};
            auto& t = reg.get<TransformComponent>(e);
            t.position.y = 1.0f;
            t.scale = {3.0f, 0.3f, 3.0f};
        }, asChild);
    });
    physics->addAction(icon(QStringLiteral("entity")), QObject::tr("Trigger Volume"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Trigger"), [](Scene& scene, entt::entity e) {
            auto& collider = scene.registry().emplace<ColliderComponent>(e);
            collider.size = {2.0f, 2.0f, 2.0f};
            collider.isTrigger = true;
            scene.registry().get<TransformComponent>(e).position.y = 1.0f;
        }, asChild);
    });
    menu.addSeparator();
    menu.addAction(icon(QStringLiteral("sun")), QObject::tr("Directional Light"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Directional Light"), [](Scene& scene, entt::entity e) {
            scene.registry().emplace<LightComponent>(e).type = LightType::Directional;
            auto& t = scene.registry().get<TransformComponent>(e);
            t.position.y = 5.0f;
            t.rotationDeg = {-45.0f, 30.0f, 0.0f};
        }, asChild);
    });
    menu.addAction(icon(QStringLiteral("light")), QObject::tr("Point Light"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Point Light"), [](Scene& scene, entt::entity e) {
            auto& light = scene.registry().emplace<LightComponent>(e);
            light.type = LightType::Point;
            light.intensity = 10.0f;
            light.range = 8.0f;
            scene.registry().get<TransformComponent>(e).position.y = 2.0f;
        }, asChild);
    });
    QMenu* effects = menu.addMenu(icon(QStringLiteral("sparkle")), QObject::tr("Effects"));
    effects->addAction(icon(QStringLiteral("sparkle")), QObject::tr("Smoke"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Smoke"), [](Scene& scene, entt::entity e) {
            auto& p = scene.registry().emplace<ParticleEmitterComponent>(e);
            p.rate = 14.0f;
            p.lifetime = 4.0f;
            p.speed = 0.8f;
            p.spreadDeg = 15.0f;
            p.shape = ParticleShape::Sphere;
            p.shapeSize = 0.2f;
            p.gravity = -0.03f;
            p.drag = 0.3f;
            p.startSize = 0.4f;
            p.endSize = 2.2f;
            p.startColor = {0.55f, 0.55f, 0.57f};
            p.endColor = {0.35f, 0.35f, 0.37f};
            p.startAlpha = 0.55f;
            p.endAlpha = 0.0f;
        }, asChild);
    });
    effects->addAction(icon(QStringLiteral("sparkle")), QObject::tr("Sparks"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Sparks"), [](Scene& scene, entt::entity e) {
            auto& p = scene.registry().emplace<ParticleEmitterComponent>(e);
            p.rate = 60.0f;
            p.lifetime = 0.9f;
            p.speed = 5.0f;
            p.speedVariation = 0.5f;
            p.spreadDeg = 40.0f;
            p.gravity = 1.0f;
            p.startSize = 0.06f;
            p.endSize = 0.02f;
            p.startColor = {1.0f, 0.8f, 0.4f};
            p.endColor = {1.0f, 0.3f, 0.05f};
            p.intensity = 6.0f;
            p.blend = ParticleBlend::Additive;
            p.endAlpha = 0.2f;
        }, asChild);
    });
    effects->addAction(icon(QStringLiteral("grid")), QObject::tr("Decal"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Decal"), [](Scene& scene, entt::entity e) {
            scene.registry().emplace<DecalComponent>(e);
            scene.registry().get<TransformComponent>(e).scale = {2.0f, 1.0f, 2.0f};
        }, asChild);
    });
    effects->addAction(icon(QStringLiteral("sparkle")), QObject::tr("Fire"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Fire"), [](Scene& scene, entt::entity e) {
            auto& p = scene.registry().emplace<ParticleEmitterComponent>(e);
            p.rate = 70.0f;
            p.lifetime = 0.8f;
            p.speed = 1.6f;
            p.spreadDeg = 12.0f;
            p.shape = ParticleShape::Sphere;
            p.shapeSize = 0.25f;
            p.gravity = -0.2f;
            p.startSize = 0.6f;
            p.endSize = 0.15f;
            p.startColor = {1.0f, 0.55f, 0.15f};
            p.endColor = {0.9f, 0.15f, 0.02f};
            p.intensity = 3.0f;
            p.blend = ParticleBlend::Additive;
            p.startAlpha = 0.8f;
            p.endAlpha = 0.0f;
        }, asChild);
    });
    menu.addAction(icon(QStringLiteral("light")), QObject::tr("Spot Light"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Spot Light"), [](Scene& scene, entt::entity e) {
            auto& light = scene.registry().emplace<LightComponent>(e);
            light.type = LightType::Spot;
            light.castShadows = true;
            light.intensity = 20.0f;
            light.range = 12.0f;
            auto& t = scene.registry().get<TransformComponent>(e);
            t.position.y = 4.0f;
            t.rotationDeg = {-90.0f, 0.0f, 0.0f};  // pointing down
        }, asChild);
    });
    menu.addAction(icon(QStringLiteral("sun")), QObject::tr("Reflection Probe"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Reflection Probe"), [](Scene& scene, entt::entity e) {
            scene.registry().emplace<ReflectionProbeComponent>(e);
            scene.registry().get<TransformComponent>(e).position.y = 2.0f;
        }, asChild);
    });
    menu.addAction(icon(QStringLiteral("camera")), QObject::tr("Camera"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Camera"), [](Scene& scene, entt::entity e) {
            scene.registry().emplace<CameraComponent>(e).primary = scene.primaryCamera() == entt::null;
            scene.registry().get<TransformComponent>(e).position = {0.0f, 2.0f, 6.0f};
        }, asChild);
    });
}

QIcon entityIcon(const Scene& scene, entt::entity e) {
    using app::theme::icon;
    const auto& reg = scene.registry();
    if (const auto* light = reg.try_get<LightComponent>(e)) {
        return icon(light->type == LightType::Directional ? QStringLiteral("sun") : QStringLiteral("light"));
    }
    if (reg.all_of<CameraComponent>(e)) return icon(QStringLiteral("camera"));
    if (const auto* mesh = reg.try_get<MeshRendererComponent>(e)) {
        switch (mesh->mesh) {
            case MeshPrimitive::Sphere: return icon(QStringLiteral("sphere"));
            case MeshPrimitive::Plane: return icon(QStringLiteral("plane"));
            case MeshPrimitive::Cylinder: return icon(QStringLiteral("cylinder"));
            default: return icon(QStringLiteral("cube"));
        }
    }
    return icon(scene.childrenOf(e).empty() ? QStringLiteral("entity") : QStringLiteral("hierarchy"));
}

} // namespace lumora::editor
