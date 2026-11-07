#include "assets/AssetImport.hpp"

#include "assets/FbxImporter.hpp"
#include "assets/GltfImporter.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <system_error>

namespace lumora {

namespace {

std::string lowerExtension(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool isGltf(const std::filesystem::path& file) {
    const std::string ext = lowerExtension(file);
    return ext == ".glb" || ext == ".gltf";
}

} // namespace

bool isModelFile(const std::filesystem::path& file) {
    const std::string ext = lowerExtension(file);
    return ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj";
}

Result<ImportedModel> importModel(const std::filesystem::path& file) {
    if (isGltf(file)) return importGltf(file);
    if (isModelFile(file)) return importFbx(file);
    return makeError("'{}' is not a model (.gltf, .glb, .fbx, .obj)", file.filename().string());
}

Result<std::vector<std::filesystem::path>> modelDependencies(const std::filesystem::path& file) {
    if (isGltf(file)) return gltfDependencies(file);
    if (isModelFile(file)) return fbxDependencies(file);
    return makeError("'{}' is not a model (.gltf, .glb, .fbx, .obj)", file.filename().string());
}

Result<std::filesystem::path> importFile(const std::filesystem::path& source,
                                         const std::filesystem::path& destinationDir) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) return makeError("'{}' is not a file", source.string());
    std::filesystem::create_directories(destinationDir, ec);
    if (ec) return makeError("cannot create '{}': {}", destinationDir.string(), ec.message());
    if (std::filesystem::equivalent(source.parent_path(), destinationDir, ec)) return source;
    std::filesystem::path target = destinationDir / source.filename();
    for (int n = 1; std::filesystem::exists(target, ec); ++n) {
        target = destinationDir / std::format("{}_{}{}", source.stem().string(), n, source.extension().string());
    }
    std::filesystem::copy_file(source, target, ec);
    if (ec) return makeError("cannot copy '{}': {}", source.string(), ec.message());
    return target;
}

Result<std::filesystem::path> importModelFile(const std::filesystem::path& source,
                                              const std::filesystem::path& destinationDir) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return makeError("'{}' is not a file", source.string());
    }
    if (!isModelFile(source)) {
        return makeError("'{}' is not a model (.gltf, .glb, .fbx, .obj)", source.filename().string());
    }
    // Validate before copying anything.
    auto deps = modelDependencies(source);
    if (!deps) return std::unexpected(deps.error());

    std::filesystem::create_directories(destinationDir, ec);
    if (ec) return makeError("cannot create '{}': {}", destinationDir.string(), ec.message());

    std::filesystem::path target = destinationDir / source.filename();
    for (int n = 1; std::filesystem::exists(target, ec); ++n) {
        target = destinationDir / std::format("{}_{}{}", source.stem().string(), n, source.extension().string());
    }
    const bool samePlace = std::filesystem::equivalent(source.parent_path(), destinationDir, ec);
    if (!samePlace || target.filename() != source.filename()) {
        std::filesystem::copy_file(source, target, ec);
        if (ec) return makeError("cannot copy '{}': {}", source.string(), ec.message());
    }
    // External resources keep their relative paths; existing identical names are left alone.
    for (const std::filesystem::path& dep : *deps) {
        const std::filesystem::path relative = dep.lexically_normal();
        if (relative.is_absolute() || (!relative.empty() && *relative.begin() == "..")) continue;
        const auto from = source.parent_path() / relative;
        const auto to = destinationDir / relative;
        if (std::filesystem::exists(to, ec)) continue;
        std::filesystem::create_directories(to.parent_path(), ec);
        std::filesystem::copy_file(from, to, ec);
        if (ec) return makeError("cannot copy dependency '{}': {}", from.string(), ec.message());
    }
    return target;
}

} // namespace lumora
