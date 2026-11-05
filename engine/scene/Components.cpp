#include "scene/Components.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace lumora {

glm::mat4 TransformComponent::localMatrix() const {
    const glm::vec3 r = glm::radians(rotationDeg);
    const glm::mat4 rotation = glm::eulerAngleYXZ(r.y, r.x, r.z);
    return glm::translate(glm::mat4(1.0f), position) * rotation * glm::scale(glm::mat4(1.0f), scale);
}

} // namespace lumora
