#include "assets/Image.hpp"

#include "core/FileSystem.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>
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

} // namespace lumora
