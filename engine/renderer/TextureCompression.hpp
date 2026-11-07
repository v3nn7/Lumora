#pragma once

#include "core/Error.hpp"
#include "renderer/Gpu.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace lumora::render {

/// What a material map holds, which decides how it is stored on the GPU.
enum class TextureKind {
    Color,   // base colour, emissive: sRGB; BC1, or BC3 with alpha
    Data,    // metallic-roughness, occlusion: linear BC1
    Normal,  // tangent-space normals: BC5 (x, y; the shader rebuilds z)
};

/// A texture with its full mip chain as the GPU takes it (block-compressed or RGBA8).
struct TextureMips {
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::vector<std::uint8_t>> levels;  // level 0 first

    [[nodiscard]] std::size_t bytes(std::uint32_t firstLevel = 0) const;
    [[nodiscard]] std::uint32_t levelWidth(std::uint32_t level) const { return std::max(width >> level, 1u); }
    [[nodiscard]] std::uint32_t levelHeight(std::uint32_t level) const { return std::max(height >> level, 1u); }
};

/// The block format for a map (RGBA8 when the GPU has no BC support).
[[nodiscard]] nvrhi::Format textureFormat(TextureKind kind, bool hasAlpha, bool blockCompression);

/// Compresses one RGBA8 image (rows top to bottom) into BC1, BC3, BC4 (red) or BC5 (red, green)
/// blocks; edges are padded by repeating the last texel.
[[nodiscard]] std::vector<std::uint8_t> compressBlocks(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                       std::uint32_t height, nvrhi::Format format);

/// Mip chain (alpha-coverage preserving where there is transparency) in the kind's format.
[[nodiscard]] TextureMips prepareTexture(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height,
                                         TextureKind kind, bool blockCompression);

/// The .ltex cache file: "LTX1", format, width, height, level count, then each level's size and bytes.
[[nodiscard]] Result<> saveTextureMips(const std::filesystem::path& file, const TextureMips& mips);
[[nodiscard]] Result<TextureMips> loadTextureMips(const std::filesystem::path& file);

} // namespace lumora::render
