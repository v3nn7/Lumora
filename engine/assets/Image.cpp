#include "assets/Image.hpp"

#include "core/FileSystem.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <string>

namespace lumora {

Result<ImageData> decodeImage(std::span<const std::byte> encoded) {
    if (encoded.empty() || encoded.size() > static_cast<std::size_t>(INT32_MAX)) {
        return makeError("image data is empty or too large");
    }
    const auto* bytes = reinterpret_cast<const stbi_uc*>(encoded.data());
    const int length = static_cast<int>(encoded.size());
    int w = 0, h = 0, channels = 0;
    if (stbi_info_from_memory(bytes, length, &w, &h, &channels) == 0) {
        return makeError("unsupported or corrupt image ({})", stbi_failure_reason());
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        return makeError("image size {}x{} is out of range", w, h);
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(bytes, length, &w, &h, &channels, 4),
                                                                &stbi_image_free);
    if (!pixels) return makeError("image decode failed ({})", stbi_failure_reason());
    ImageData image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    std::memcpy(image.rgba.data(), pixels.get(), image.rgba.size());
    return image;
}

Result<ImageData> loadImageFile(const std::filesystem::path& file) {
    auto bytes = fs::readBinaryFile(file);
    if (!bytes) return std::unexpected(bytes.error());
    auto image = decodeImage(*bytes);
    if (!image) return makeError("{}: {}", file.filename().string(), image.error().message);
    return image;
}

bool isImageFile(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
}

Result<HdrImage> loadHdrFile(const std::filesystem::path& file) {
    auto bytes = fs::readBinaryFile(file);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->empty() || bytes->size() > static_cast<std::size_t>(INT32_MAX)) {
        return makeError("{}: file is empty or too large", file.filename().string());
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes->data());
    const int length = static_cast<int>(bytes->size());
    if (stbi_is_hdr_from_memory(data, length) == 0) {
        return makeError("{}: not a Radiance HDR image", file.filename().string());
    }
    int w = 0, h = 0, channels = 0;
    std::unique_ptr<float, decltype(&stbi_image_free)> pixels(stbi_loadf_from_memory(data, length, &w, &h, &channels, 3),
                                                              &stbi_image_free);
    if (!pixels) return makeError("{}: HDR decode failed ({})", file.filename().string(), stbi_failure_reason());
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        return makeError("{}: image size {}x{} is out of range", file.filename().string(), w, h);
    }
    HdrImage image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.rgb.assign(pixels.get(), pixels.get() + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    return image;
}

bool isHdrFile(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".hdr";
}

Result<> saveHdrFile(const std::filesystem::path& file, const HdrImage& image) {
    if (image.width == 0 || image.height == 0 || image.rgb.size() != static_cast<std::size_t>(image.width) * image.height * 3) {
        return makeError("{}: invalid HDR image", file.filename().string());
    }
    const std::string header = std::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {} +X {}\n", image.height, image.width);
    std::vector<std::byte> bytes(header.size() + static_cast<std::size_t>(image.width) * image.height * 4);
    std::memcpy(bytes.data(), header.data(), header.size());
    std::byte* out = bytes.data() + header.size();
    for (std::size_t i = 0; i < static_cast<std::size_t>(image.width) * image.height; ++i) {
        const float r = std::max(image.rgb[i * 3], 0.0f), g = std::max(image.rgb[i * 3 + 1], 0.0f), b = std::max(image.rgb[i * 3 + 2], 0.0f);
        const float v = std::max({r, g, b});
        std::uint8_t rgbe[4] = {0, 0, 0, 0};
        if (v >= 1e-32f && std::isfinite(v)) {
            int e = 0;
            const float scale = std::frexp(v, &e) * 256.0f / v;
            rgbe[0] = static_cast<std::uint8_t>(std::min(r * scale, 255.0f));
            rgbe[1] = static_cast<std::uint8_t>(std::min(g * scale, 255.0f));
            rgbe[2] = static_cast<std::uint8_t>(std::min(b * scale, 255.0f));
            rgbe[3] = static_cast<std::uint8_t>(std::clamp(e + 128, 0, 255));
        }
        std::memcpy(out + i * 4, rgbe, 4);
    }
    if (std::error_code ec; !std::filesystem::create_directories(file.parent_path(), ec) && ec) {
        return makeError("{}: {}", file.parent_path().string(), ec.message());
    }
    return fs::writeFileAtomic(file, bytes);
}

} // namespace lumora
