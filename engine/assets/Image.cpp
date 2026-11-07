#include "assets/Image.hpp"

#include "core/FileSystem.hpp"

#include <basisu_transcoder.h>
#include <stb_image.h>
#include <tinyexr.h>
#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>

namespace lumora {

namespace {

constexpr std::uint32_t kMaxImageSide = 16384;

bool startsWith(std::span<const std::byte> data, std::span<const std::uint8_t> magic, std::size_t offset = 0) {
    if (data.size() < offset + magic.size()) return false;
    return std::memcmp(data.data() + offset, magic.data(), magic.size()) == 0;
}

bool isWebp(std::span<const std::byte> data) {
    static constexpr std::array<std::uint8_t, 4> riff{'R', 'I', 'F', 'F'};
    static constexpr std::array<std::uint8_t, 4> webp{'W', 'E', 'B', 'P'};
    return startsWith(data, riff) && startsWith(data, webp, 8);
}

bool isKtx2(std::span<const std::byte> data) {
    static constexpr std::array<std::uint8_t, 12> magic{0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    return startsWith(data, magic);
}

bool isExr(std::span<const std::byte> data) {
    static constexpr std::array<std::uint8_t, 4> magic{0x76, 0x2F, 0x31, 0x01};
    return startsWith(data, magic);
}

std::string lowerExtension(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

Result<ImageData> decodeWebp(std::span<const std::byte> encoded) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    int w = 0, h = 0;
    if (WebPGetInfo(bytes, encoded.size(), &w, &h) == 0) return makeError("corrupt WebP image");
    if (w <= 0 || h <= 0 || w > static_cast<int>(kMaxImageSide) || h > static_cast<int>(kMaxImageSide)) {
        return makeError("image size {}x{} is out of range", w, h);
    }
    ImageData image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    if (WebPDecodeRGBAInto(bytes, encoded.size(), image.rgba.data(), image.rgba.size(), w * 4) == nullptr) {
        return makeError("WebP decode failed");
    }
    return image;
}

/// KTX2 with Basis Universal (ETC1S or UASTC) payload: the first image's top level, as RGBA8.
Result<ImageData> decodeKtx2(std::span<const std::byte> encoded) {
    static std::once_flag initialized;
    std::call_once(initialized, [] { basist::basisu_transcoder_init(); });
    basist::ktx2_transcoder ktx2;
    if (!ktx2.init(encoded.data(), static_cast<std::uint32_t>(encoded.size()))) {
        return makeError("unsupported KTX2 texture (only Basis Universal ETC1S/UASTC payloads are read)");
    }
    if (ktx2.is_hdr()) return makeError("HDR KTX2 textures are not supported");
    const std::uint32_t w = ktx2.get_width(), h = ktx2.get_height();
    if (w == 0 || h == 0 || w > kMaxImageSide || h > kMaxImageSide) return makeError("image size {}x{} is out of range", w, h);
    if (!ktx2.start_transcoding()) return makeError("KTX2 transcoding failed to start");
    ImageData image;
    image.width = w;
    image.height = h;
    image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    if (!ktx2.transcode_image_level(0, 0, 0, image.rgba.data(), w * h, basist::transcoder_texture_format::cTFRGBA32)) {
        return makeError("KTX2 transcoding failed");
    }
    return image;
}

Result<HdrImage> decodeExr(std::span<const std::byte> encoded) {
    float* rgba = nullptr;
    int w = 0, h = 0;
    const char* error = nullptr;
    if (LoadEXRFromMemory(&rgba, &w, &h, reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(), &error) !=
        TINYEXR_SUCCESS) {
        std::string message = error != nullptr ? error : "unknown error";
        if (error != nullptr) FreeEXRErrorMessage(error);
        return makeError("EXR decode failed ({})", message);
    }
    std::unique_ptr<float, decltype(&std::free)> pixels(rgba, &std::free);
    if (w <= 0 || h <= 0 || w > static_cast<int>(kMaxImageSide) || h > static_cast<int>(kMaxImageSide)) {
        return makeError("image size {}x{} is out of range", w, h);
    }
    HdrImage image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    image.rgb.resize(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = pixels.get()[i * 4 + static_cast<std::size_t>(c)];
            image.rgb[i * 3 + static_cast<std::size_t>(c)] = std::isfinite(v) ? std::max(v, 0.0f) : 0.0f;
        }
    }
    return image;
}

} // namespace

Result<ImageData> decodeImage(std::span<const std::byte> encoded) {
    if (encoded.empty() || encoded.size() > static_cast<std::size_t>(INT32_MAX)) {
        return makeError("image data is empty or too large");
    }
    if (isWebp(encoded)) return decodeWebp(encoded);
    if (isKtx2(encoded)) return decodeKtx2(encoded);
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
    const std::string ext = lowerExtension(file);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".webp" ||
           ext == ".ktx2";
}

Result<HdrImage> loadHdrFile(const std::filesystem::path& file) {
    auto bytes = fs::readBinaryFile(file);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->empty() || bytes->size() > static_cast<std::size_t>(INT32_MAX)) {
        return makeError("{}: file is empty or too large", file.filename().string());
    }
    if (isExr(*bytes)) {
        auto exr = decodeExr(*bytes);
        if (!exr) return makeError("{}: {}", file.filename().string(), exr.error().message);
        return exr;
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes->data());
    const int length = static_cast<int>(bytes->size());
    if (stbi_is_hdr_from_memory(data, length) == 0) {
        return makeError("{}: not a Radiance HDR or OpenEXR image", file.filename().string());
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
    const std::string ext = lowerExtension(file);
    return ext == ".hdr" || ext == ".exr";
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
