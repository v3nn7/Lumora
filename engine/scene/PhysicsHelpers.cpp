#include "scene/PhysicsHelpers.hpp"

#include <glm/common.hpp>

namespace lumora {

void fitColliderToMesh(ColliderComponent& collider, const MeshRendererComponent& mesh, const Aabb* assetBounds) {
    collider.offset = glm::vec3(0.0f);
    if (!mesh.meshAsset.empty()) {
        if (assetBounds == nullptr) return;
        collider.shape = ColliderShape::Box;
        collider.size = glm::max(assetBounds->max - assetBounds->min, glm::vec3(0.01f));
        collider.offset = (assetBounds->max + assetBounds->min) * 0.5f;
        return;
    }
    switch (mesh.mesh) {
        case MeshPrimitive::Sphere:
            collider.shape = ColliderShape::Sphere;
            collider.radius = 0.5f;
            break;
        case MeshPrimitive::Plane:
            collider.shape = ColliderShape::Box;
            collider.size = {1.0f, 0.02f, 1.0f};
            collider.offset = {0.0f, -0.01f, 0.0f};
            break;
        case MeshPrimitive::Cube:
        case MeshPrimitive::Cylinder:
        case MeshPrimitive::Count:
            collider.shape = ColliderShape::Box;
            collider.size = glm::vec3(1.0f);
            break;
    }
}

} // namespace lumora
