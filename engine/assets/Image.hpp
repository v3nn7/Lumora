#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace lumora {

/// Decoded 8-bit RGBA image (rows top to bottom).
struct ImageData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

/// Decodes PNG, JPEG, TGA or BMP from memory. Images larger than 16384 px per side are rejected.
[[nodiscard]] Result<ImageData> decodeImage(std::span<const std::byte> encoded);
[[nodiscard]] Result<ImageData> loadImageFile(const std::filesystem::path& file);
[[nodiscard]] bool isImageFile(const std::filesystem::path& file);

/// High dynamic range image: linear RGB radiance, 3 floats per pixel (rows top to bottom).
struct HdrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgb;
};

/// Loads a Radiance .hdr file (equirectangular environment maps).
[[nodiscard]] Result<HdrImage> loadHdrFile(const std::filesystem::path& file);
[[nodiscard]] bool isHdrFile(const std::filesystem::path& file);

} // namespace lumora
