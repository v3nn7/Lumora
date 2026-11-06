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
    menu.addAction(icon(QStringLiteral("light")), QObject::tr("Spot Light"), [&ctx, asChild] {
        ctx.createEntity(QObject::tr("Spot Light"), [](Scene& scene, entt::entity e) {
            auto& light = scene.registry().emplace<LightComponent>(e);
            light.type = LightType::Spot;
            light.intensity = 20.0f;
            light.range = 12.0f;
            auto& t = scene.registry().get<TransformComponent>(e);
            t.position.y = 4.0f;
            t.rotationDeg = {-90.0f, 0.0f, 0.0f};  // pointing down
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
