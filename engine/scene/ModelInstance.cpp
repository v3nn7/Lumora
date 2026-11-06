#include "scene/ModelInstance.hpp"

#include "assets/AssetLibrary.hpp"
#include "scene/Scene.hpp"

#include <glm/gtx/euler_angles.hpp>

#include <vector>

namespace lumora {

namespace {

void addMeshRenderer(Scene& scene, entt::entity e, const ImportedModel& model, std::string_view relativeFile, int meshIndex,
                     int skin) {
    const ImportedMesh& mesh = model.meshes[static_cast<std::size_t>(meshIndex)];
    auto& renderer = scene.registry().emplace<MeshRendererComponent>(e);
    renderer.meshAsset = AssetLibrary::meshKey(relativeFile, meshIndex);
    if (skin >= 0 && !mesh.data.skin.empty()) scene.registry().emplace<SkinComponent>(e).skin = skin;
    if (mesh.material < 0 || mesh.material >= static_cast<int>(model.materials.size())) return;
    const ImportedMaterial& m = model.materials[static_cast<std::size_t>(mesh.material)];
    renderer.baseColor = glm::vec3(m.baseColor);
    renderer.metallic = m.metallic;
    renderer.roughness = std::max(m.roughness, 0.02f);
    renderer.emissive = m.emissive;
    renderer.emissiveStrength = glm::length(m.emissive) > 0.0f ? 1.0f : 0.0f;
    auto texture = [&](int image) { return image >= 0 ? AssetLibrary::textureKey(relativeFile, image) : std::string{}; };
    renderer.baseColorTexture = texture(m.baseColorTexture);
    renderer.metallicRoughnessTexture = texture(m.metallicRoughnessTexture);
    renderer.normalTexture = texture(m.normalTexture);
    renderer.emissiveTexture = texture(m.emissiveTexture);
    // Emissive maps multiply the factor; make sure a map alone still shows up.
    if (m.emissiveTexture >= 0 && renderer.emissiveStrength == 0.0f) renderer.emissiveStrength = 1.0f;
}

} // namespace

entt::entity instantiateModel(Scene& scene, const ImportedModel& model, std::string_view relativeFile,
                              std::string_view rootName, entt::entity parent) {
    const entt::entity root = scene.createEntity(rootName, Uuid::generate(), parent);
    std::vector<entt::entity> created(model.nodes.size(), entt::entity{entt::null});
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const ImportedNode& node = model.nodes[i];
        const entt::entity nodeParent = node.parent >= 0 ? created[static_cast<std::size_t>(node.parent)] : root;
        const entt::entity e = scene.createEntity(node.name, Uuid::generate(), nodeParent);
        created[i] = e;

        scene.registry().emplace<ModelNodeComponent>(e).node = static_cast<int>(i);
        auto& t = scene.registry().get<TransformComponent>(e);
        t.position = node.translation;
        t.scale = node.scale;
        float y = 0.0f, x = 0.0f, z = 0.0f;
        glm::extractEulerAngleYXZ(glm::mat4_cast(glm::normalize(node.rotation)), y, x, z);
        t.rotationDeg = glm::degrees(glm::vec3(x, y, z));

        if (node.mesh >= 0) {
            // A mesh with several materials is several parts: the first on the node's entity,
            // the others on child entities (same place, own material).
            std::vector<int> parts = static_cast<std::size_t>(node.mesh) < model.meshParts.size()
                                         ? model.meshParts[static_cast<std::size_t>(node.mesh)]
                                         : std::vector<int>{node.mesh};
            for (std::size_t p = 0; p < parts.size(); ++p) {
                if (parts[p] < 0 || parts[p] >= static_cast<int>(model.meshes.size())) continue;
                const entt::entity target = p == 0 ? e : scene.createEntity(model.meshes[static_cast<std::size_t>(parts[p])].name,
                                                                             Uuid::generate(), e);
                addMeshRenderer(scene, target, model, relativeFile, parts[p], node.skin);
            }
        }
    }
    scene.updateWorldTransforms();
    if (!model.animations.empty()) {
        auto& animator = scene.registry().emplace<AnimatorComponent>(root);
        animator.model = std::string(relativeFile);
    }
    scene.updateWorldTransforms();
    return root;
}

} // namespace lumora
