#include "renderer/TextureCompression.hpp"

#include "core/FileSystem.hpp"
#include "core/JobSystem.hpp"

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include <algorithm>
#include <cstring>

namespace lumora::render {

namespace {

constexpr char kMagic[4] = {'L', 'T', 'X', '1'};

std::uint32_t blockBytes(nvrhi::Format format) {
    return format == nvrhi::Format::BC1_UNORM || format == nvrhi::Format::BC1_UNORM_SRGB || format == nvrhi::Format::BC4_UNORM ? 8u : 16u;
}

bool knownFormat(nvrhi::Format f) {
    switch (f) {
        case nvrhi::Format::BC1_UNORM:
        case nvrhi::Format::BC1_UNORM_SRGB:
        case nvrhi::Format::BC3_UNORM:
        case nvrhi::Format::BC3_UNORM_SRGB:
        case nvrhi::Format::BC4_UNORM:
        case nvrhi::Format::BC5_UNORM:
        case nvrhi::Format::RGBA8_UNORM:
        case nvrhi::Format::SRGBA8_UNORM:
            return true;
        default:
            return false;
    }
}

std::size_t levelBytes(nvrhi::Format format, std::uint32_t w, std::uint32_t h) {
    if (format == nvrhi::Format::RGBA8_UNORM || format == nvrhi::Format::SRGBA8_UNORM) return static_cast<std::size_t>(w) * h * 4;
    return static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * blockBytes(format);
}

} // namespace

std::size_t TextureMips::bytes(std::uint32_t firstLevel) const {
    std::size_t total = 0;
    for (std::size_t i = firstLevel; i < levels.size(); ++i) total += levels[i].size();
    return total;
}

nvrhi::Format textureFormat(TextureKind kind, bool hasAlpha, bool blockCompression) {
    if (!blockCompression) return kind == TextureKind::Color ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
    switch (kind) {
        case TextureKind::Color: return hasAlpha ? nvrhi::Format::BC3_UNORM_SRGB : nvrhi::Format::BC1_UNORM_SRGB;
        case TextureKind::Data: return nvrhi::Format::BC1_UNORM;
        case TextureKind::Normal: return nvrhi::Format::BC5_UNORM;
    }
    return nvrhi::Format::RGBA8_UNORM;
}

std::vector<std::uint8_t> compressBlocks(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height,
                                         nvrhi::Format format) {
    const std::uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    const std::uint32_t size = blockBytes(format);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bx) * by * size);
    auto row = [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = 0; x < bx; ++x) {
                unsigned char block[64];
                for (std::uint32_t j = 0; j < 4; ++j) {
                    for (std::uint32_t i = 0; i < 4; ++i) {
                        const std::uint32_t sx = std::min(x * 4 + i, width - 1), sy = std::min(y * 4 + j, height - 1);
                        std::memcpy(block + (j * 4 + i) * 4, &rgba[(static_cast<std::size_t>(sy) * width + sx) * 4], 4);
                    }
                }
                unsigned char* dst = &out[(static_cast<std::size_t>(y) * bx + x) * size];
                switch (format) {
                    case nvrhi::Format::BC1_UNORM:
                    case nvrhi::Format::BC1_UNORM_SRGB:
                        stb_compress_dxt_block(dst, block, 0, STB_DXT_HIGHQUAL);
                        break;
                    case nvrhi::Format::BC3_UNORM:
                    case nvrhi::Format::BC3_UNORM_SRGB:
                        stb_compress_dxt_block(dst, block, 1, STB_DXT_HIGHQUAL);
                        break;
                    case nvrhi::Format::BC4_UNORM: {
                        unsigned char red[16];
                        for (int k = 0; k < 16; ++k) red[k] = block[k * 4];
                        stb_compress_bc4_block(dst, red);
                        break;
                    }
                    default: {  // BC5: red and green
                        unsigned char rg[32];
                        for (int k = 0; k < 16; ++k) {
                            rg[k * 2] = block[k * 4];
                            rg[k * 2 + 1] = block[k * 4 + 1];
                        }
                        stb_compress_bc5_block(dst, rg);
                        break;
                    }
                }
            }
        }
    };
    JobSystem::global().parallelFor(by, 8, [&](std::size_t begin, std::size_t end) {
        row(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end));
    });
    return out;
}

TextureMips prepareTexture(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height, TextureKind kind,
                           bool blockCompression) {
    bool hasAlpha = false;
    for (std::size_t i = 3; i < rgba.size() && !hasAlpha; i += 4) hasAlpha = rgba[i] < 255;
    TextureMips out;
    out.format = textureFormat(kind, hasAlpha, blockCompression);
    out.width = width;
    out.height = height;
    auto chain = buildMipChain(rgba, width, height, kind == TextureKind::Color);
    if (out.format == nvrhi::Format::RGBA8_UNORM || out.format == nvrhi::Format::SRGBA8_UNORM) {
        out.levels = std::move(chain);
        return out;
    }
    for (std::size_t l = 0; l < chain.size(); ++l) {
        out.levels.push_back(compressBlocks(chain[l], out.levelWidth(static_cast<std::uint32_t>(l)),
                                            out.levelHeight(static_cast<std::uint32_t>(l)), out.format));
    }
    return out;
}

Result<> saveTextureMips(const std::filesystem::path& file, const TextureMips& mips) {
    std::vector<std::byte> bytes(4 + 16);
    std::memcpy(bytes.data(), kMagic, 4);
    const std::uint32_t header[4] = {static_cast<std::uint32_t>(mips.format), mips.width, mips.height,
                                     static_cast<std::uint32_t>(mips.levels.size())};
    std::memcpy(bytes.data() + 4, header, 16);
    for (const auto& level : mips.levels) {
        const auto size = static_cast<std::uint32_t>(level.size());
        const std::size_t at = bytes.size();
        bytes.resize(at + 4 + level.size());
        std::memcpy(bytes.data() + at, &size, 4);
        std::memcpy(bytes.data() + at + 4, level.data(), level.size());
    }
    if (std::error_code ec; !std::filesystem::create_directories(file.parent_path(), ec) && ec) {
        return makeError("{}: {}", file.parent_path().string(), ec.message());
    }
    return fs::writeFileAtomic(file, bytes);
}

Result<TextureMips> loadTextureMips(const std::filesystem::path& file) {
    auto bytes = fs::readBinaryFile(file);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() < 20 || std::memcmp(bytes->data(), kMagic, 4) != 0) return makeError("{}: not a texture cache file", file.filename().string());
    std::uint32_t header[4];
    std::memcpy(header, bytes->data() + 4, 16);
    TextureMips mips;
    mips.format = static_cast<nvrhi::Format>(header[0]);
    mips.width = header[1];
    mips.height = header[2];
    if (!knownFormat(mips.format) || mips.width == 0 || mips.height == 0 || mips.width > 16384 || mips.height > 16384 || header[3] == 0 ||
        header[3] > 15) {
        return makeError("{}: damaged texture cache file", file.filename().string());
    }
    std::size_t at = 20;
    for (std::uint32_t l = 0; l < header[3]; ++l) {
        std::uint32_t size = 0;
        if (at + 4 > bytes->size()) return makeError("{}: truncated", file.filename().string());
        std::memcpy(&size, bytes->data() + at, 4);
        at += 4;
        if (size != levelBytes(mips.format, mips.levelWidth(l), mips.levelHeight(l)) || at + size > bytes->size()) {
            return makeError("{}: damaged texture cache file", file.filename().string());
        }
        const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes->data() + at);
        mips.levels.emplace_back(begin, begin + size);
        at += size;
    }
    return mips;
}

} // namespace lumora::render
