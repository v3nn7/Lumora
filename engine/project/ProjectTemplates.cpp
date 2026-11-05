#include "project/ProjectTemplates.hpp"

#include <format>

namespace lumora {

namespace {

entt::entity addSun(Scene& scene) {
    const entt::entity sun = scene.createEntity("Sun");
    auto& t = scene.registry().get<TransformComponent>(sun);
    t.position = {0.0f, 10.0f, 0.0f};
    t.rotationDeg = {-38.0f, 40.0f, 0.0f};
    auto& light = scene.registry().emplace<LightComponent>(sun);
    light.type = LightType::Directional;
    light.color = {1.0f, 0.93f, 0.84f};
    light.intensity = 3.6f;
    return sun;
}

entt::entity addCamera(Scene& scene, glm::vec3 position, glm::vec3 rotationDeg) {
    const entt::entity cam = scene.createEntity("Main Camera");
    auto& t = scene.registry().get<TransformComponent>(cam);
    t.position = position;
    t.rotationDeg = rotationDeg;
    scene.registry().emplace<CameraComponent>(cam);
    return cam;
}

entt::entity addMesh(Scene& scene, std::string_view name, MeshPrimitive prim, glm::vec3 pos, glm::vec3 color,
                     float metallic, float roughness, entt::entity parent = entt::null) {
    const entt::entity e = scene.createEntity(name, Uuid::generate(), parent);
    scene.registry().get<TransformComponent>(e).position = pos;
    auto& m = scene.registry().emplace<MeshRendererComponent>(e);
    m.mesh = prim;
    m.baseColor = color;
    m.metallic = metallic;
    m.roughness = roughness;
    return e;
}

entt::entity addGround(Scene& scene, float size) {
    const entt::entity ground = addMesh(scene, "Ground", MeshPrimitive::Plane, {0, 0, 0}, {0.42f, 0.42f, 0.43f}, 0.0f, 0.85f);
    scene.registry().get<TransformComponent>(ground).scale = {size, 1.0f, size};
    scene.registry().get<MeshRendererComponent>(ground).pattern = SurfacePattern::Grid;
    auto& collider = scene.registry().emplace<ColliderComponent>(ground);  // static ground for physics
    collider.size = {1.0f, 0.02f, 1.0f};
    collider.offset = {0.0f, -0.01f, 0.0f};
    return ground;
}

} // namespace

std::unique_ptr<Scene> createTemplateScene(ProjectTemplate templ) {
    auto scene = std::make_unique<Scene>("Main");
    switch (templ) {
        case ProjectTemplate::Empty: {
            addSun(*scene);
            addCamera(*scene, {0.0f, 2.0f, 6.0f}, {-12.0f, 0.0f, 0.0f});
            break;
        }
        case ProjectTemplate::BasicScene: {
            addSun(*scene);
            addCamera(*scene, {0.0f, 2.5f, 7.0f}, {-15.0f, 0.0f, 0.0f});
            addGround(*scene, 1000.0f);
            const entt::entity group = scene->createEntity("Props");
            const entt::entity cube = addMesh(*scene, "Cube", MeshPrimitive::Cube, {-1.8f, 0.5f, 0.0f},
                                              {0.85f, 0.25f, 0.18f}, 0.0f, 0.4f, group);
            scene->registry().get<TransformComponent>(cube).rotationDeg = {0.0f, 25.0f, 0.0f};
            scene->registry().emplace<RotatorComponent>(cube);
            addMesh(*scene, "Sphere", MeshPrimitive::Sphere, {0.0f, 0.75f, 0.0f}, {0.95f, 0.78f, 0.35f}, 1.0f,
                    0.25f, group);
            scene->registry().get<TransformComponent>(scene->childrenOf(group).back()).scale = glm::vec3(1.5f);
            addMesh(*scene, "Cylinder", MeshPrimitive::Cylinder, {1.8f, 0.6f, 0.0f}, {0.2f, 0.45f, 0.85f}, 0.0f,
                    0.6f, group);
            scene->registry().get<TransformComponent>(scene->childrenOf(group).back()).scale = {1.0f, 1.2f, 1.0f};
            const entt::entity lamp = scene->createEntity("Fill Light");
            scene->registry().get<TransformComponent>(lamp).position = {2.5f, 2.0f, 2.5f};
            auto& l = scene->registry().emplace<LightComponent>(lamp);
            l.type = LightType::Point;
            l.color = {0.4f, 0.6f, 1.0f};
            l.intensity = 12.0f;
            l.range = 8.0f;
            break;
        }
        case ProjectTemplate::MaterialShowcase: {
            addSun(*scene);
            addCamera(*scene, {0.0f, 3.2f, 9.5f}, {-14.0f, 0.0f, 0.0f});
            addGround(*scene, 1000.0f);
            const entt::entity grid = scene->createEntity("Material Grid");
            constexpr int kCount = 6;
            for (int row = 0; row < 2; ++row) {
                for (int i = 0; i < kCount; ++i) {
                    const float t = static_cast<float>(i) / (kCount - 1);
                    const float metallic = row == 0 ? 0.0f : 1.0f;
                    const glm::vec3 color = row == 0 ? glm::vec3(0.75f, 0.12f, 0.10f) : glm::vec3(0.95f, 0.80f, 0.45f);
                    addMesh(*scene, std::format("Sphere M{:.0f} R{:.1f}", metallic, std::max(t, 0.05f)),
                            MeshPrimitive::Sphere, {(i - (kCount - 1) * 0.5f) * 1.25f, 0.5f + row * 1.25f, 0.0f},
                            color, metallic, std::max(t, 0.05f), grid);
                }
            }
            break;
        }
        case ProjectTemplate::PhysicsPlayground: {
            addSun(*scene);
            addCamera(*scene, {7.0f, 4.5f, 9.0f}, {-20.0f, 38.0f, 0.0f});
            addGround(*scene, 1000.0f);
            auto body = [&](entt::entity e, ColliderShape shape, float mass) {
                auto& c = scene->registry().emplace<ColliderComponent>(e);
                c.shape = shape;
                auto& rb = scene->registry().emplace<RigidBodyComponent>(e);
                rb.mass = mass;
            };
            // Tower of crates.
            const entt::entity tower = scene->createEntity("Crate Tower");
            for (int level = 0; level < 6; ++level) {
                for (int i = 0; i < 2; ++i) {
                    const bool alongX = level % 2 == 0;
                    const float offset = (static_cast<float>(i) - 0.5f) * 1.02f;
                    const glm::vec3 pos = alongX ? glm::vec3(offset, 0.5f + 1.0f * static_cast<float>(level), 0.0f)
                                                 : glm::vec3(0.0f, 0.5f + 1.0f * static_cast<float>(level), offset);
                    const entt::entity crate = addMesh(*scene, std::format("Crate {}-{}", level, i), MeshPrimitive::Cube, pos,
                                                       {0.50f, 0.33f, 0.18f}, 0.0f, 0.8f, tower);
                    body(crate, ColliderShape::Box, 8.0f);
                }
            }
            // Balls dropped from above.
            const entt::entity balls = scene->createEntity("Balls");
            for (int i = 0; i < 5; ++i) {
                const entt::entity ball = addMesh(*scene, std::format("Ball {}", i), MeshPrimitive::Sphere,
                                                  {-3.0f + 0.35f * static_cast<float>(i), 4.0f + 1.4f * static_cast<float>(i), -1.5f + 0.6f * static_cast<float>(i)},
                                                  {0.95f, 0.45f - 0.07f * static_cast<float>(i), 0.12f}, 0.0f, 0.35f, balls);
                body(ball, ColliderShape::Sphere, 2.0f);
                scene->registry().get<RigidBodyComponent>(ball).restitution = 0.45f;
            }
            // Kinematic paddle sweeping through the tower.
            const entt::entity paddle = addMesh(*scene, "Spinning Paddle", MeshPrimitive::Cube, {0.0f, 0.6f, 0.0f},
                                                {0.85f, 0.85f, 0.9f}, 1.0f, 0.3f);
            auto& pt = scene->registry().get<TransformComponent>(paddle);
            pt.position = {3.3f, 0.6f, 0.0f};
            pt.scale = {6.0f, 0.5f, 0.4f};
            body(paddle, ColliderShape::Box, 1.0f);
            scene->registry().get<RigidBodyComponent>(paddle).motion = BodyMotion::Kinematic;
            scene->registry().emplace<RotatorComponent>(paddle).speedDeg = 40.0f;
            break;
        }
    }
    scene->updateWorldTransforms();
    return scene;
}

} // namespace lumora
