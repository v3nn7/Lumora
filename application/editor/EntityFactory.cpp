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
