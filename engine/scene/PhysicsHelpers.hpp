#pragma once

#include "assets/MeshData.hpp"
#include "scene/Components.hpp"

namespace lumora {

/// Chooses a collider matching a mesh: sphere for spheres, box around everything else
/// (local-space bounds, so the entity's scale still applies). `assetBounds` is used for
/// imported meshes; pass nullptr for built-in primitives.
void fitColliderToMesh(ColliderComponent& collider, const MeshRendererComponent& mesh, const Aabb* assetBounds);

} // namespace lumora
