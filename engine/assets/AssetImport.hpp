#pragma once

#include "core/Error.hpp"

#include <filesystem>
#include <vector>

namespace lumora {

struct ImportedModel;

/// glTF (.gltf, .glb), FBX (.fbx) or Wavefront OBJ (.obj).
[[nodiscard]] bool isModelFile(const std::filesystem::path& file);
/// Loads any model file isModelFile accepts.
[[nodiscard]] Result<ImportedModel> importModel(const std::filesystem::path& file);
/// Files next to a model that it reads (glTF buffers and images, an OBJ's material library,
/// textures), relative to the model's folder.
[[nodiscard]] Result<std::vector<std::filesystem::path>> modelDependencies(const std::filesystem::path& file);

/// Copies a model file into `destinationDir` (created if needed), together with the external
/// buffers, material libraries and images it references (keeping their relative layout). Never overwrites: an
/// existing name gets a numeric suffix. Returns the copied model's path.
[[nodiscard]] Result<std::filesystem::path> importModelFile(const std::filesystem::path& source,
                                                            const std::filesystem::path& destinationDir);

/// Copies a single file (e.g. a texture) into `destinationDir` without overwriting.
[[nodiscard]] Result<std::filesystem::path> importFile(const std::filesystem::path& source,
                                                       const std::filesystem::path& destinationDir);

} // namespace lumora
