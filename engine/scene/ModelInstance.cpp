#include "scene/ModelInstance.hpp"

#include "assets/AssetLibrary.hpp"
#include "scene/Scene.hpp"

#include <glm/gtx/euler_angles.hpp>

#include <vector>

namespace lumora {

entt::entity instantiateModel(Scene& scene, const ImportedModel& model, std::string_view relativeFile,
                              std::string_view rootName, entt::entity parent) {
    const entt::entity root = scene.createEntity(rootName, Uuid::generate(), parent);
    std::vector<entt::entity> created(model.nodes.size(), entt::entity{entt::null});
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const ImportedNode& node = model.nodes[i];
        const entt::entity nodeParent = node.parent >= 0 ? created[static_cast<std::size_t>(node.parent)] : root;
        const entt::entity e = scene.createEntity(node.name, Uuid::generate(), nodeParent);
        created[i] = e;

        auto& t = scene.registry().get<TransformComponent>(e);
        t.position = node.translation;
        t.scale = node.scale;
        float y = 0.0f, x = 0.0f, z = 0.0f;
        glm::extractEulerAngleYXZ(glm::mat4_cast(glm::normalize(node.rotation)), y, x, z);
        t.rotationDeg = glm::degrees(glm::vec3(x, y, z));

        if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size())) {
            const ImportedMesh& mesh = model.meshes[static_cast<std::size_t>(node.mesh)];
            auto& renderer = scene.registry().emplace<MeshRendererComponent>(e);
            renderer.meshAsset = AssetLibrary::meshKey(relativeFile, node.mesh);
            if (mesh.material >= 0 && mesh.material < static_cast<int>(model.materials.size())) {
                const ImportedMaterial& m = model.materials[static_cast<std::size_t>(mesh.material)];
                renderer.baseColor = glm::vec3(m.baseColor);
                renderer.metallic = m.metallic;
                renderer.roughness = std::max(m.roughness, 0.02f);
                renderer.emissive = m.emissive;
                renderer.emissiveStrength = glm::length(m.emissive) > 0.0f ? 1.0f : 0.0f;
            }
        }
    }
    scene.updateWorldTransforms();
    return root;
}

} // namespace lumora
