#include "renderer/Gpu.hpp"

#include "assets/MeshData.hpp"
#include "core/FileSystem.hpp"
#include "renderer/GpuTypes.hpp"
#include "renderer/HudRenderer.hpp"
#include "renderer/Overlay.hpp"
#include "renderer/vulkan/VulkanCommon.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace lumora::render {

bool isSrgbFormat(nvrhi::Format format) noexcept {
    switch (format) {
        case nvrhi::Format::SRGBA8_UNORM:
        case nvrhi::Format::SBGRA8_UNORM:
        case nvrhi::Format::SBGRX8_UNORM:
            return true;
        default:
            return false;
    }
}

// ---- Bindless ------------------------------------------------------------------------------

BindlessTable::BindlessTable(nvrhi::IDevice* device, bool d3d12) : device_(device) {
    nvrhi::BindlessLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::All;
    desc.firstSlot = 0;
    desc.maxCapacity = kCapacity;
    // Binding 0: Texture2D gTextures[] (D3D12: t0 in register space 1); D3D12 maps the table to
    // space 2 as well, for Texture3D gVolumes[] (Vulkan aliases them on one binding).
    desc.registerSpaces = {nvrhi::BindingLayoutItem::Texture_SRV(1)};
    if (d3d12) desc.registerSpaces.push_back(nvrhi::BindingLayoutItem::Texture_SRV(2));
    layout_ = device_->createBindlessLayout(desc);
    if (!layout_) throw rhi::VulkanError("could not create the bindless descriptor layout", VK_ERROR_INITIALIZATION_FAILED);
    table_ = device_->createDescriptorTable(layout_);
    device_->resizeDescriptorTable(table_, kCapacity, false);
}

std::uint32_t BindlessTable::registerTexture(nvrhi::ITexture* texture, nvrhi::Format viewFormat) {
    std::scoped_lock lock(mutex_);
    std::uint32_t index = 0;
    if (!freeTextures_.empty()) {
        index = freeTextures_.back();
        freeTextures_.pop_back();
    } else {
        if (nextTexture_ >= kCapacity) throw rhi::VulkanError("bindless texture table is full", VK_ERROR_OUT_OF_POOL_MEMORY);
        index = nextTexture_++;
    }
    device_->writeDescriptorTable(table_, nvrhi::BindingSetItem::Texture_SRV(index, texture, viewFormat));
    return index;
}

void BindlessTable::releaseTexture(std::uint32_t index) {
    if (index == UINT32_MAX) return;
    std::scoped_lock lock(mutex_);
    freeTextures_.push_back(index);
}

// ---- Shaders ---------------------------------------------------------------------------------

ShaderLibrary::ShaderLibrary(nvrhi::IDevice* device, std::filesystem::path directory, std::string extension)
    : device_(device), directory_(std::move(directory)), extension_(std::move(extension)) {}

nvrhi::IShader* ShaderLibrary::get(std::string_view name, const char* entry) {
    const std::string key = std::string(name) + "|" + entry;
    std::scoped_lock lock(mutex_);
    if (auto it = shaders_.find(key); it != shaders_.end()) return it->second;
    const auto path = directory_ / (std::string(name) + extension_);
    std::ifstream in(path, std::ios::binary);
    if (!in) throw rhi::VulkanError("shader not found: " + path.string(), VK_ERROR_INITIALIZATION_FAILED);
    const std::vector<char> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    nvrhi::ShaderType type = nvrhi::ShaderType::Vertex;
    if (name.ends_with(".frag")) type = nvrhi::ShaderType::Pixel;
    if (name.ends_with(".comp")) type = nvrhi::ShaderType::Compute;
    nvrhi::ShaderDesc desc;
    desc.shaderType = type;
    desc.entryName = entry;
    desc.debugName = key;
    nvrhi::ShaderHandle shader = device_->createShader(desc, code.data(), code.size());
    if (!shader) throw rhi::VulkanError("could not create shader " + key, VK_ERROR_INITIALIZATION_FAILED);
    return shaders_.emplace(key, shader).first->second;
}

// ---- Uploads ---------------------------------------------------------------------------------

void writeMapped(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, std::span<const std::byte> bytes) {
    if (bytes.empty()) return;
    void* mapped = device->mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
    if (mapped == nullptr) throw rhi::VulkanError("could not map a frame buffer", VK_ERROR_MEMORY_MAP_FAILED);
    std::memcpy(mapped, bytes.data(), bytes.size());
    device->unmapBuffer(buffer);
}

namespace {

const std::array<float, 256>& srgbToLinearTable() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<std::size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

std::uint8_t linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(s * 255.0f));
}

} // namespace

namespace {

/// Mips of an image with transparency (cut-out leaves, fences). Plain box filtering would mix
/// the colour hidden under transparent texels into the edges and smear alpha so that the
/// cut-out grows or vanishes with distance. Instead:
///  - colours are averaged weighted by alpha (in linear light for sRGB), so only visible texels
///    contribute;
///  - transparent texels take the colour of their surroundings (pulled from coarser levels), so
///    bilinear filtering at an edge never blends in the background;
///  - each level's alpha is scaled so the share of texels above the 0.5 cutoff matches the full
///    image (Castano's coverage-preserving mips): leaves keep their size at any distance.
std::vector<std::vector<std::uint8_t>> transparentMipChain(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                           std::uint32_t height, bool srgb) {
    const auto& toLinear = srgbToLinearTable();
    struct Level {
        std::uint32_t w, h;
        std::vector<float> rgb;    // linear (or raw) colour, alpha-weighted average
        std::vector<float> alpha;  // 0..1, unscaled box average
    };
    std::vector<Level> levels;
    {
        Level base{width, height, std::vector<float>(static_cast<std::size_t>(width) * height * 3),
                   std::vector<float>(static_cast<std::size_t>(width) * height)};
        for (std::size_t i = 0; i < base.alpha.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                base.rgb[i * 3 + c] = srgb ? toLinear[rgba[i * 4 + c]] : static_cast<float>(rgba[i * 4 + c]) / 255.0f;
            }
            base.alpha[i] = static_cast<float>(rgba[i * 4 + 3]) / 255.0f;
        }
        levels.push_back(std::move(base));
    }
    while (levels.back().w > 1 || levels.back().h > 1) {
        const Level& src = levels.back();
        Level dst{std::max(src.w / 2, 1u), std::max(src.h / 2, 1u), {}, {}};
        dst.rgb.assign(static_cast<std::size_t>(dst.w) * dst.h * 3, 0.0f);
        dst.alpha.assign(static_cast<std::size_t>(dst.w) * dst.h, 0.0f);
        for (std::uint32_t y = 0; y < dst.h; ++y) {
            for (std::uint32_t x = 0; x < dst.w; ++x) {
                const std::uint32_t xs[2] = {std::min(x * 2, src.w - 1), std::min(x * 2 + 1, src.w - 1)};
                const std::uint32_t ys[2] = {std::min(y * 2, src.h - 1), std::min(y * 2 + 1, src.h - 1)};
                float weight = 0.0f, alpha = 0.0f, rgb[3] = {0, 0, 0}, plain[3] = {0, 0, 0};
                for (const std::uint32_t sy : ys) {
                    for (const std::uint32_t sx : xs) {
                        const std::size_t t = static_cast<std::size_t>(sy) * src.w + sx;
                        const float a = src.alpha[t];
                        alpha += a;
                        weight += a;
                        for (int c = 0; c < 3; ++c) {
                            rgb[c] += src.rgb[t * 3 + c] * a;
                            plain[c] += src.rgb[t * 3 + c];
                        }
                    }
                }
                const std::size_t o = static_cast<std::size_t>(y) * dst.w + x;
                dst.alpha[o] = alpha * 0.25f;
                for (int c = 0; c < 3; ++c) dst.rgb[o * 3 + c] = weight > 1e-6f ? rgb[c] / weight : plain[c] * 0.25f;
            }
        }
        levels.push_back(std::move(dst));
    }
    // Transparent texels: the colour around them, from the next coarser level (already filled).
    for (std::size_t l = levels.size() - 1; l-- > 0;) {
        Level& level = levels[l];
        const Level& parent = levels[l + 1];
        for (std::uint32_t y = 0; y < level.h; ++y) {
            for (std::uint32_t x = 0; x < level.w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * level.w + x;
                if (level.alpha[i] > 1.0f / 255.0f) continue;
                const std::size_t p = static_cast<std::size_t>(std::min(y / 2, parent.h - 1)) * parent.w + std::min(x / 2, parent.w - 1);
                for (int c = 0; c < 3; ++c) level.rgb[i * 3 + c] = parent.rgb[p * 3 + c];
            }
        }
    }
    // Coverage of the 0.5 cutoff in the full image, kept in every level.
    constexpr float kCutoff = 0.5f;
    auto coverage = [&](const std::vector<float>& alpha, float scale) {
        std::size_t above = 0;
        for (const float a : alpha) above += a * scale >= kCutoff ? 1 : 0;
        return static_cast<float>(above) / static_cast<float>(std::max<std::size_t>(alpha.size(), 1));
    };
    const float target = coverage(levels[0].alpha, 1.0f);
    std::vector<std::vector<std::uint8_t>> out;
    out.reserve(levels.size());
    for (std::size_t l = 0; l < levels.size(); ++l) {
        const Level& level = levels[l];
        float scale = 1.0f;
        if (l > 0 && target > 0.0f && target < 1.0f) {
            float lo = 0.0f, hi = 8.0f;
            for (int step = 0; step < 16; ++step) {
                const float mid = (lo + hi) * 0.5f;
                (coverage(level.alpha, mid) < target ? lo : hi) = mid;
            }
            scale = hi;
        }
        std::vector<std::uint8_t> bytes(level.alpha.size() * 4);
        for (std::size_t i = 0; i < level.alpha.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                const float v = level.rgb[i * 3 + c];
                bytes[i * 4 + c] = srgb ? linearToSrgb(v) : static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            const float a = l == 0 ? static_cast<float>(rgba[i * 4 + 3]) / 255.0f : std::clamp(level.alpha[i] * scale, 0.0f, 1.0f);
            bytes[i * 4 + 3] = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
        }
        out.push_back(std::move(bytes));
    }
    return out;
}

} // namespace

std::vector<std::vector<std::uint8_t>> buildMipChain(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                     std::uint32_t height, bool srgb) {
    bool transparent = false;
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] < 255) {
            transparent = true;
            break;
        }
    }
    if (transparent && width > 1 && height > 1) return transparentMipChain(rgba, width, height, srgb);
    std::vector<std::vector<std::uint8_t>> levels;
    levels.emplace_back(rgba.begin(), rgba.end());
    const auto& toLinear = srgbToLinearTable();
    std::uint32_t w = width, h = height;
    while (w > 1 || h > 1) {
        const std::uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        const std::vector<std::uint8_t>& src = levels.back();
        std::vector<std::uint8_t> dst(static_cast<std::size_t>(nw) * nh * 4);
        for (std::uint32_t y = 0; y < nh; ++y) {
            for (std::uint32_t x = 0; x < nw; ++x) {
                const std::uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const std::uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                const std::size_t taps[4] = {(static_cast<std::size_t>(y0) * w + x0) * 4, (static_cast<std::size_t>(y0) * w + x1) * 4,
                                             (static_cast<std::size_t>(y1) * w + x0) * 4, (static_cast<std::size_t>(y1) * w + x1) * 4};
                const std::size_t out = (static_cast<std::size_t>(y) * nw + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    if (srgb && c < 3) {
                        float sum = 0.0f;
                        for (const std::size_t t : taps) sum += toLinear[src[t + c]];
                        dst[out + c] = linearToSrgb(sum * 0.25f);
                    } else {
                        unsigned sum = 0;
                        for (const std::size_t t : taps) sum += src[t + c];
                        dst[out + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                    }
                }
            }
        }
        levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return levels;
}

} // namespace lumora::render
