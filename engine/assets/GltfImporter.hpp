#pragma once

#include "assets/MeshData.hpp"
#include "core/Error.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace lumora {

/// glTF metallic-roughness material. Texture fields index ImportedModel::images (-1 = none).
struct ImportedMaterial {
    std::string name;
    glm::vec4 baseColor{1.0f};
    float metallic = 1.0f;   // glTF defaults
    float roughness = 1.0f;
    glm::vec3 emissive{0.0f};
    int baseColorTexture = -1;
    int metallicRoughnessTexture = -1;
    int emissiveTexture = -1;
    int normalTexture = -1;
};

/// Still-encoded image (PNG/JPEG) taken from the model: embedded in a .glb buffer, a data URI,
/// or a file next to the model. Decoded lazily by the AssetLibrary.
struct ImportedImage {
    std::string name;
    std::string mimeType;
    std::vector<std::byte> encoded;
};

struct ImportedMesh {
    std::string name;
    MeshData data;           // all triangle primitives merged
    int material = -1;       // material of the first primitive (single material per mesh for now)
};

/// Joints (node indices) deforming skinned meshes, with their inverse bind matrices.
struct ImportedSkin {
    std::string name;
    std::vector<int> joints;
    std::vector<glm::mat4> inverseBind;
};

struct ImportedNode {
    std::string name;
    int parent = -1;
    int mesh = -1;
    int skin = -1;  // index into ImportedModel::skins when the node's mesh is skinned
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

/// One animated property of one node. Values are vec3 (translation, scale) or quaternions
/// stored x, y, z, w (rotation); cubic-spline channels hold in-tangent, value, out-tangent per key.
struct AnimationChannel {
    enum class Path { Translation, Rotation, Scale };
    enum class Interpolation { Step, Linear, CubicSpline };
    int node = -1;  // index into ImportedModel::nodes
    Path path = Path::Translation;
    Interpolation interpolation = Interpolation::Linear;
    std::vector<float> times;       // seconds, ascending
    std::vector<glm::vec4> values;
};

struct ImportedAnimation {
    std::string name;
    float duration = 0.0f;  // last key time
    std::vector<AnimationChannel> channels;
};

/// Local pose of one node, as sampled from an animation.
struct NodePose {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

/// Writes the animated properties at `time` (seconds, clamped to the clip) into `poses`
/// (indexed like ImportedModel::nodes); properties no channel animates are left untouched.
void sampleAnimation(const ImportedAnimation& animation, float time, std::vector<NodePose>& poses);

/// A loaded glTF 2.0 asset (.gltf with external or embedded buffers, or .glb).
/// Coordinate conventions match Lumora (right-handed, +Y up, metres, CCW front faces).
struct ImportedModel {
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedNode> nodes;  // parents precede children
    std::vector<ImportedImage> images;
    std::vector<ImportedAnimation> animations;
    std::vector<ImportedSkin> skins;
    std::vector<std::string> warnings;
};

[[nodiscard]] Result<ImportedModel> importGltf(const std::filesystem::path& file);

/// Files a model depends on (external buffers and images referenced by relative URI), so an
/// importer can copy them next to the model. Data URIs and .glb chunks are not listed.
[[nodiscard]] Result<std::vector<std::filesystem::path>> gltfDependencies(const std::filesystem::path& file);

} // namespace lumora
