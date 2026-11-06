#include "renderer/EnvironmentMap.hpp"

#include "core/JobSystem.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lumora::render {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

using Level = EnvironmentMap::Level;

/// 2x2 box downsample (odd sizes clamp); the poles keep their rows.
Level downsample(const Level& src) {
    Level dst;
    dst.width = std::max(src.width / 2, 1u);
    dst.height = std::max(src.height / 2, 1u);
    dst.rgb.resize(static_cast<std::size_t>(dst.width) * dst.height * 3);
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            glm::vec3 sum(0.0f);
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sx = std::min(x * 2 + dx, src.width - 1);
                    const std::uint32_t sy = std::min(y * 2 + dy, src.height - 1);
                    const float* p = &src.rgb[(static_cast<std::size_t>(sy) * src.width + sx) * 3];
                    sum += glm::vec3(p[0], p[1], p[2]);
                }
            }
            float* out = &dst.rgb[(static_cast<std::size_t>(y) * dst.width + x) * 3];
            out[0] = sum.x * 0.25f;
            out[1] = sum.y * 0.25f;
            out[2] = sum.z * 0.25f;
        }
    }
    return dst;
}

/// Bilinear sample at (u, v) in [0,1]: u wraps around, v clamps at the poles.
glm::vec3 sampleBilinear(const Level& level, float u, float v) {
    const float fx = u * static_cast<float>(level.width) - 0.5f;
    const float fy = std::clamp(v * static_cast<float>(level.height) - 0.5f, 0.0f, static_cast<float>(level.height - 1));
    const float x0f = std::floor(fx);
    const float y0f = std::floor(fy);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const auto w = static_cast<int>(level.width);
    const int x0 = ((static_cast<int>(x0f) % w) + w) % w;
    const int x1 = (x0 + 1) % w;
    const int y0 = static_cast<int>(y0f);
    const int y1 = std::min(y0 + 1, static_cast<int>(level.height) - 1);
    auto at = [&](int x, int y) {
        const float* p = &level.rgb[(static_cast<std::size_t>(y) * level.width + static_cast<std::size_t>(x)) * 3];
        return glm::vec3(p[0], p[1], p[2]);
    };
    return glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx), glm::mix(at(x0, y1), at(x1, y1), tx), ty);
}

/// Trilinear sample of a mip pyramid at a fractional level.
glm::vec3 samplePyramid(const std::vector<Level>& pyramid, float u, float v, float lod) {
    lod = std::clamp(lod, 0.0f, static_cast<float>(pyramid.size() - 1));
    const auto l0 = static_cast<std::size_t>(lod);
    const std::size_t l1 = std::min(l0 + 1, pyramid.size() - 1);
    const float t = lod - static_cast<float>(l0);
    const glm::vec3 a = sampleBilinear(pyramid[l0], u, v);
    return t > 0.0f ? glm::mix(a, sampleBilinear(pyramid[l1], u, v), t) : a;
}

glm::vec2 directionToUv(const glm::vec3& d) {
    const float u = 0.5f + std::atan2(d.x, -d.z) / (2.0f * kPi);
    const float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPi;
    return {u, v};
}

float radicalInverse(std::uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

/// Radiance of the GGX lobe (roughness r) around n = v = r, by filtered importance sampling
/// (Karis 2013, Colbert & Krivanek 2007): each sample reads the pyramid at the mip whose texel
/// covers the solid angle the sample stands for, so few samples give a smooth result.
glm::vec3 prefilter(const std::vector<Level>& pyramid, const glm::vec3& n, float roughness, std::uint32_t samples) {
    const float alpha = roughness * roughness;
    const glm::vec3 up = std::abs(n.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 tx = glm::normalize(glm::cross(up, n));
    const glm::vec3 ty = glm::cross(n, tx);
    const float texelSolidAngle =
        4.0f * kPi / static_cast<float>(static_cast<std::size_t>(pyramid[0].width) * pyramid[0].height);
    glm::vec3 sum(0.0f);
    float weight = 0.0f;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const float e1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
        const float e2 = radicalInverse(i);
        const float phi = 2.0f * kPi * e2;
        const float cosTheta = std::sqrt((1.0f - e1) / (1.0f + (alpha * alpha - 1.0f) * e1));
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const glm::vec3 h = glm::normalize(tx * (sinTheta * std::cos(phi)) + ty * (sinTheta * std::sin(phi)) + n * cosTheta);
        const glm::vec3 l = 2.0f * glm::dot(n, h) * h - n;
        const float nDotL = glm::dot(n, l);
        if (nDotL <= 0.0f) continue;
        const float nDotH = std::max(glm::dot(n, h), 0.0f);
        const float a2 = alpha * alpha;
        const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
        const float D = a2 / std::max(kPi * d * d, 1e-7f);
        const float pdf = D * 0.25f + 1e-4f;  // D * NdotH / (4 VdotH) with n = v
        const float sampleSolidAngle = 1.0f / (static_cast<float>(samples) * pdf);
        const float lod = alpha <= 0.0f ? 0.0f : 0.5f * std::log2(sampleSolidAngle / texelSolidAngle) + 1.0f;
        const glm::vec2 uv = directionToUv(l);
        sum += samplePyramid(pyramid, uv.x, uv.y, lod) * nDotL;
        weight += nDotL;
    }
    return weight > 0.0f ? sum / weight : glm::vec3(0.0f);
}

/// Real SH basis up to l = 2 (9 functions).
std::array<float, 9> shBasis(const glm::vec3& d) {
    return {0.282095f,
            0.488603f * d.y,
            0.488603f * d.z,
            0.488603f * d.x,
            1.092548f * d.x * d.y,
            1.092548f * d.y * d.z,
            0.315392f * (3.0f * d.z * d.z - 1.0f),
            1.092548f * d.x * d.z,
            0.546274f * (d.x * d.x - d.y * d.y)};
}

} // namespace

glm::vec3 equirectDirection(float u, float v) {
    const float phi = (u - 0.5f) * 2.0f * kPi;  // inverse of directionToUv
    const float theta = v * kPi;
    const float s = std::sin(theta);
    return {s * std::sin(phi), std::cos(theta), -s * std::cos(phi)};
}

glm::vec3 evaluateIrradiance(const std::array<glm::vec3, 9>& sh, const glm::vec3& n) {
    const auto basis = shBasis(n);
    glm::vec3 result(0.0f);
    for (std::size_t i = 0; i < 9; ++i) result += sh[i] * basis[i];
    return glm::max(result, glm::vec3(0.0f));
}

EnvironmentMap prefilterEnvironment(const HdrImage& image) {
    EnvironmentMap env;
    // Source pyramid: full resolution down to a few texels.
    std::vector<Level> pyramid;
    pyramid.push_back({image.width, image.height, image.rgb});
    while (pyramid.back().width > 4 && pyramid.back().height > 2) pyramid.push_back(downsample(pyramid.back()));

    // Base of the specular chain: the pyramid level at most 512 wide (or the image itself).
    std::size_t base = 0;
    while (base + 1 < pyramid.size() && pyramid[base].width > 512) ++base;
    const std::vector<Level> source(pyramid.begin() + static_cast<std::ptrdiff_t>(base), pyramid.end());

    env.specular.resize(kEnvironmentSpecularLevels);
    env.specular[0] = source[0];
    for (std::uint32_t level = 1; level < kEnvironmentSpecularLevels; ++level) {
        Level& out = env.specular[level];
        // Exactly the GPU mip chain of the base (max(1, size >> level)): uploaded as its mips.
        out.width = std::max(source[0].width >> level, 1u);
        out.height = std::max(source[0].height >> level, 1u);
        out.rgb.resize(static_cast<std::size_t>(out.width) * out.height * 3);
        const float roughness = static_cast<float>(level) / static_cast<float>(kEnvironmentSpecularLevels - 1);
        const std::uint32_t samples = level < 3 ? 96u : 64u;
        JobSystem::global().parallelFor(out.height, 2, [&](std::size_t y0, std::size_t y1) {
            for (std::size_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = 0; x < out.width; ++x) {
                    const glm::vec3 n = equirectDirection((static_cast<float>(x) + 0.5f) / static_cast<float>(out.width),
                                                          (static_cast<float>(y) + 0.5f) / static_cast<float>(out.height));
                    const glm::vec3 c = prefilter(source, n, roughness, samples);
                    float* p = &out.rgb[(y * out.width + x) * 3];
                    p[0] = c.x;
                    p[1] = c.y;
                    p[2] = c.z;
                }
            }
        });
    }

    // Irradiance: project a small level onto SH9 (solid-angle weighted), then convolve with the
    // clamped cosine (Ramamoorthi & Hanrahan 2001) and divide by pi.
    const Level& small = pyramid[std::min<std::size_t>(pyramid.size() - 1, base + 2)];
    std::array<glm::vec3, 9> sh{};
    for (std::uint32_t y = 0; y < small.height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(small.height);
        const float solidAngle = (2.0f * kPi / static_cast<float>(small.width)) * (kPi / static_cast<float>(small.height)) *
                                 std::sin(v * kPi);
        for (std::uint32_t x = 0; x < small.width; ++x) {
            const glm::vec3 d = equirectDirection((static_cast<float>(x) + 0.5f) / static_cast<float>(small.width), v);
            const float* p = &small.rgb[(static_cast<std::size_t>(y) * small.width + x) * 3];
            const glm::vec3 radiance(p[0], p[1], p[2]);
            const auto basis = shBasis(d);
            for (std::size_t i = 0; i < 9; ++i) sh[i] += radiance * (basis[i] * solidAngle);
        }
    }
    constexpr std::array<float, 9> kCosineBand{kPi, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f,
                                               kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f};
    for (std::size_t i = 0; i < 9; ++i) env.irradiance[i] = sh[i] * (kCosineBand[i] / kPi);
    return env;
}

} // namespace lumora::render
