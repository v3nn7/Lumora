#include "scene/Camera.hpp"

#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cmath>

namespace lumora {

glm::mat4 perspectiveReverseZ(float fovYRadians, float aspect, float nearPlane) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = -f;  // Vulkan clip space has +Y pointing down.
    m[2][3] = -1.0f;
    m[3][2] = nearPlane;
    return m;
}

CameraView makeCameraView(const glm::mat4& cameraWorld, float fovDeg, float nearPlane, float aspect) {
    CameraView v;
    // Strip scale so a scaled camera entity does not distort the view.
    glm::mat4 rigid = cameraWorld;
    rigid[0] = glm::vec4(glm::normalize(glm::vec3(cameraWorld[0])), 0.0f);
    rigid[1] = glm::vec4(glm::normalize(glm::vec3(cameraWorld[1])), 0.0f);
    rigid[2] = glm::vec4(glm::normalize(glm::vec3(cameraWorld[2])), 0.0f);
    v.view = glm::inverse(rigid);
    v.fovYRadians = glm::radians(std::clamp(fovDeg, 1.0f, 179.0f));
    v.nearPlane = std::max(nearPlane, 1e-4f);
    v.aspect = aspect > 0.0f ? aspect : 1.0f;
    v.projection = perspectiveReverseZ(v.fovYRadians, v.aspect, v.nearPlane);
    v.position = glm::vec3(cameraWorld[3]);
    return v;
}

CameraView sceneCameraView(const Scene& scene, float aspect) {
    const entt::entity cam = scene.primaryCamera();
    if (cam == entt::null) {
        FlyCameraController fallback;
        fallback.setPosition({0.0f, 3.0f, 8.0f});
        fallback.lookAt({0.0f, 0.5f, 0.0f});
        return fallback.view(aspect);
    }
    const auto& c = scene.registry().get<CameraComponent>(cam);
    return makeCameraView(scene.worldMatrix(cam), c.fovDeg, c.nearPlane, aspect);
}

void FlyCameraController::setRotation(float yawDeg, float pitchDeg) noexcept {
    yaw_ = std::fmod(yawDeg, 360.0f);
    pitch_ = std::clamp(pitchDeg, -89.0f, 89.0f);
}

void FlyCameraController::lookAt(const glm::vec3& target) noexcept {
    const glm::vec3 d = target - position_;
    const float len = glm::length(d);
    if (len < 1e-6f) return;
    const glm::vec3 n = d / len;
    // forward = (-sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch))
    setRotation(glm::degrees(std::atan2(-n.x, -n.z)), glm::degrees(std::asin(std::clamp(n.y, -1.0f, 1.0f))));
}

void FlyCameraController::setSpeed(float s) noexcept { speed_ = std::clamp(s, 0.05f, 500.0f); }

glm::vec3 FlyCameraController::forward() const noexcept {
    const float y = glm::radians(yaw_);
    const float p = glm::radians(pitch_);
    return {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
}

glm::vec3 FlyCameraController::right() const noexcept {
    const float y = glm::radians(yaw_);
    return {std::cos(y), 0.0f, -std::sin(y)};
}

glm::mat4 FlyCameraController::worldMatrix() const noexcept {
    return glm::translate(glm::mat4(1.0f), position_) *
           glm::eulerAngleYX(glm::radians(yaw_), glm::radians(pitch_));
}

CameraView FlyCameraController::view(float aspect) const {
    return makeCameraView(worldMatrix(), fovDeg, nearPlane, aspect);
}

void FlyCameraController::update(const InputState& input, float dt) {
    const bool looking = input.isDown(MouseButton::Right);
    const glm::vec2 md = input.mouseDelta();
    if (looking) {
        setRotation(yaw_ - md.x * lookSensitivity, pitch_ - md.y * lookSensitivity);
        if (input.scrollDelta() != 0.0f) {
            setSpeed(speed_ * std::pow(1.2f, input.scrollDelta()));
        }
        glm::vec3 move{0.0f};
        if (input.isDown(Key::W) || input.isDown(Key::Up)) move += forward();
        if (input.isDown(Key::S) || input.isDown(Key::Down)) move -= forward();
        if (input.isDown(Key::D) || input.isDown(Key::Right)) move += right();
        if (input.isDown(Key::A) || input.isDown(Key::Left)) move -= right();
        if (input.isDown(Key::E) || input.isDown(Key::Space)) move.y += 1.0f;
        if (input.isDown(Key::Q) || input.isDown(Key::LeftCtrl)) move.y -= 1.0f;
        if (glm::dot(move, move) > 0.0f) {
            const float boost = input.isDown(Key::LeftShift) ? 3.0f : 1.0f;
            position_ += glm::normalize(move) * speed_ * boost * dt;
        }
    } else {
        if (input.isDown(MouseButton::Middle)) {
            const glm::vec3 up = glm::cross(right(), forward());
            const float panScale = speed_ * 0.0025f;
            position_ += (-right() * md.x + up * md.y) * panScale;
        }
        if (input.scrollDelta() != 0.0f) {
            position_ += forward() * input.scrollDelta() * std::max(speed_ * 0.25f, 0.1f);
        }
    }
}

} // namespace lumora
