#pragma once

#include "assets/GltfImporter.hpp"

#include <entt/entity/entity.hpp>

#include <string_view>

namespace lumora {

class Scene;

/// Creates entities mirroring an imported model's node hierarchy under a new root entity.
/// Mesh nodes get a MeshRendererComponent referencing "<relativeFile>#<mesh>" and the mesh's
/// material factors. Returns the root entity.
entt::entity instantiateModel(Scene& scene, const ImportedModel& model, std::string_view relativeFile,
                              std::string_view rootName, entt::entity parent = entt::null);

} // namespace lumora
