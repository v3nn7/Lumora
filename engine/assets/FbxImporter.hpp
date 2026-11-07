#pragma once

#include "assets/GltfImporter.hpp"

namespace lumora {

/// Loads an FBX (binary or ASCII) or Wavefront OBJ (with its .mtl) through ufbx into the same
/// ImportedModel as glTF: +Y up, right-handed; `unitScale` carries the file's units (FBX in
/// centimetres: 0.01). Meshes are split per material, skins and animation stacks (baked to
/// linear keys) come along. Textures are read from the file (embedded) or found next to it.
[[nodiscard]] Result<ImportedModel> importFbx(const std::filesystem::path& file);

/// Relative paths (to the model's folder) of the files an FBX or OBJ reads: its material
/// library and the textures found next to it.
[[nodiscard]] Result<std::vector<std::filesystem::path>> fbxDependencies(const std::filesystem::path& file);

/// The files an FBX, OBJ or MTL names, present or not, relative to its folder (texture paths as
/// written when they stay inside it, otherwise the bare file name): what to fetch from a peer.
[[nodiscard]] std::vector<std::filesystem::path> fbxReferences(const std::filesystem::path& file);

} // namespace lumora
