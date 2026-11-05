#pragma once

#include "input/InputState.hpp"

#include <entt/entity/entity.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace lumora {

class Scene;

/// Everything the renderer needs to know about the point of view.
/// Projection is Vulkan-style: Y down in clip space, reversed infinite Z (near -> 1, far -> 0).
struct CameraView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 position{0.0f};
    float fovYRadians = 1.0f;
    float nearPlane = 0.05f;
    float aspect = 1.0f;
};

[[nodiscard]] glm::mat4 perspectiveReverseZ(float fovYRadians, float aspect, float nearPlane);
[[nodiscard]] CameraView makeCameraView(const glm::mat4& cameraWorld, float fovDeg, float nearPlane, float aspect);
/// View from the scene's primary camera entity, or a default framing if none exists.
[[nodiscard]] CameraView sceneCameraView(const Scene& scene, float aspect);

/// Unreal-style editor/free camera: hold RMB to look + WASDQE to fly, scroll adjusts speed,
/// MMB drag pans, scroll without RMB dollies.
class FlyCameraController {
public:
    void setPosition(const glm::vec3& p) noexcept { position_ = p; }
    void setRotation(float yawDeg, float pitchDeg) noexcept;
    void lookAt(const glm::vec3& target) noexcept;
    void update(const InputState& input, float dt);

    [[nodiscard]] const glm::vec3& position() const noexcept { return position_; }
    [[nodiscard]] float yawDeg() const noexcept { return yaw_; }
    [[nodiscard]] float pitchDeg() const noexcept { return pitch_; }
    [[nodiscard]] float speed() const noexcept { return speed_; }
    void setSpeed(float s) noexcept;
    [[nodiscard]] glm::vec3 forward() const noexcept;
    [[nodiscard]] glm::vec3 right() const noexcept;
    [[nodiscard]] glm::mat4 worldMatrix() const noexcept;
    [[nodiscard]] CameraView view(float aspect) const;

    float fovDeg = 60.0f;
    float nearPlane = 0.05f;
    float lookSensitivity = 0.15f;  // degrees per pixel

private:
    glm::vec3 position_{0.0f, 2.0f, 6.0f};
    float yaw_ = 0.0f;    // 0 = looking down -Z
    float pitch_ = -10.0f;
    float speed_ = 4.0f;  // m/s
};

} // namespace lumora
