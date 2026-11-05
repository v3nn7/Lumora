#include "scene/Camera.hpp"
#include "scene/Picking.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

using namespace lumora;

TEST(Camera, ReverseZProjectionMapsNearToOneAndFarToZero) {
    const glm::mat4 p = perspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f);
    glm::vec4 nearClip = p * glm::vec4(0, 0, -0.1f, 1);
    glm::vec4 farClip = p * glm::vec4(0, 0, -10000.0f, 1);
    EXPECT_NEAR(nearClip.z / nearClip.w, 1.0f, 1e-6f);
    EXPECT_NEAR(farClip.z / farClip.w, 0.0f, 1e-4f);
    // +Y in view space maps to -Y in Vulkan clip space.
    glm::vec4 up = p * glm::vec4(0, 1, -5, 1);
    EXPECT_LT(up.y / up.w, 0.0f);
}

TEST(Camera, FlyControllerLookAtFacesTarget) {
    FlyCameraController cam;
    cam.setPosition({0, 0, 5});
    cam.lookAt({0, 0, 0});
    EXPECT_NEAR(cam.forward().z, -1.0f, 1e-5f);
    cam.lookAt({5, 0, 5});
    EXPECT_NEAR(cam.forward().x, 1.0f, 1e-5f);
    const glm::vec3 r = cam.right();
    EXPECT_NEAR(glm::dot(r, cam.forward()), 0.0f, 1e-5f);
}

TEST(Camera, FlyControllerMovesOnlyWhileLooking) {
    FlyCameraController cam;
    cam.setPosition({0, 0, 0});
    cam.setRotation(0, 0);
    cam.setSpeed(2.0f);
    InputState input;
    input.setKey(Key::W, true);
    cam.update(input, 1.0f);
    EXPECT_FLOAT_EQ(cam.position().z, 0.0f) << "WASD requires RMB held";
    input.setMouseButton(MouseButton::Right, true);
    cam.update(input, 1.0f);
    EXPECT_NEAR(cam.position().z, -2.0f, 1e-5f);
}

TEST(Camera, KeyboardFlightWithoutLooking) {
    FlyCameraController cam;
    cam.keyboardFlight = true;
    cam.setPosition({0, 0, 0});
    cam.setRotation(0, 0);
    cam.setSpeed(2.0f);
    InputState input;
    input.setKey(Key::W, true);
    cam.update(input, 1.0f);
    EXPECT_NEAR(cam.position().z, -2.0f, 1e-5f) << "W flies forward without RMB";
    input.setKey(Key::LeftCtrl, true);  // Ctrl+S / Ctrl+W style shortcuts must not move
    cam.update(input, 1.0f);
    EXPECT_NEAR(cam.position().z, -2.0f, 1e-5f);
    input.setKey(Key::LeftCtrl, false);
    input.setKey(Key::W, false);
    input.setKey(Key::E, true);
    cam.update(input, 0.5f);
    EXPECT_NEAR(cam.position().y, 1.0f, 1e-5f) << "E flies up";
}

TEST(Camera, ViewMatrixMatchesControllerOrientation) {
    FlyCameraController cam;
    cam.setPosition({1, 2, 3});
    cam.setRotation(30, -20);
    const CameraView v = cam.view(1.5f);
    // A point one unit in front of the camera lies on the view-space -Z axis.
    const glm::vec4 p = v.view * glm::vec4(cam.position() + cam.forward(), 1.0f);
    EXPECT_NEAR(p.x, 0.0f, 1e-5f);
    EXPECT_NEAR(p.y, 0.0f, 1e-5f);
    EXPECT_NEAR(p.z, -1.0f, 1e-5f);
}

TEST(Picking, RayHitsCenteredCube) {
    Scene scene;
    const auto cube = scene.createEntity("Cube");
    scene.registry().emplace<MeshRendererComponent>(cube).mesh = MeshPrimitive::Cube;
    const auto far = scene.createEntity("Far Cube");
    scene.registry().emplace<MeshRendererComponent>(far).mesh = MeshPrimitive::Cube;
    scene.registry().get<TransformComponent>(far).position = {0, 0, -10};
    scene.updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0, 0, 5});
    cam.lookAt({0, 0, 0});
    const Ray ray = rayFromNdc(cam.view(1.0f), {0.0f, 0.0f});
    EXPECT_NEAR(ray.direction.z, -1.0f, 1e-4f);
    EXPECT_EQ(pickEntity(scene, ray), cube) << "nearest hit wins";
    const Ray miss = rayFromNdc(cam.view(1.0f), {0.95f, 0.95f});
    EXPECT_EQ(pickEntity(scene, miss), entt::entity{entt::null});
}

TEST(Picking, RayAabbSlabTest) {
    const Aabb box{{-1, -1, -1}, {1, 1, 1}};
    auto hit = intersectRayAabb({{0, 0, 5}, {0, 0, -1}}, box);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(*hit, 4.0f, 1e-5f);
    EXPECT_FALSE(intersectRayAabb({{0, 3, 5}, {0, 0, -1}}, box).has_value());
    EXPECT_FALSE(intersectRayAabb({{0, 0, 5}, {0, 0, 1}}, box).has_value());
}
