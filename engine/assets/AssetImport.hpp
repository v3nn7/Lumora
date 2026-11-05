#pragma once

#include "core/Error.hpp"

#include <filesystem>

namespace lumora {

[[nodiscard]] bool isModelFile(const std::filesystem::path& file);

/// Copies a model file into `destinationDir` (created if needed), together with the external
/// buffers/images a .gltf references (keeping their relative layout). Never overwrites: an
/// existing name gets a numeric suffix. Returns the copied model's path.
[[nodiscard]] Result<std::filesystem::path> importModelFile(const std::filesystem::path& source,
                                                            const std::filesystem::path& destinationDir);

/// Copies a single file (e.g. a texture) into `destinationDir` without overwriting.
[[nodiscard]] Result<std::filesystem::path> importFile(const std::filesystem::path& source,
                                                       const std::filesystem::path& destinationDir);

} // namespace lumora
