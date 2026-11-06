// Image-based lighting: CPU prefiltering of equirectangular HDR environments.
#include "renderer/EnvironmentMap.hpp"

#include <glm/geometric.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <numbers>

using namespace lumora;
using namespace lumora::render;

namespace {

HdrImage makeEnvironment(std::uint32_t w, std::uint32_t h, const std::function<glm::vec3(glm::vec3)>& radiance) {
    HdrImage image;
    image.width = w;
    image.height = h;
    image.rgb.resize(static_cast<std::size_t>(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const glm::vec3 c = radiance(equirectDirection((x + 0.5f) / w, (y + 0.5f) / h));
            float* p = &image.rgb[(static_cast<std::size_t>(y) * w + x) * 3];
            p[0] = c.x;
            p[1] = c.y;
            p[2] = c.z;
        }
    }
    return image;
}

/// Brute-force irradiance / pi for a normal: (1/pi) * integral of L(l) max(n.l, 0) dl.
glm::vec3 bruteIrradiance(const std::function<glm::vec3(glm::vec3)>& radiance, glm::vec3 n) {
    constexpr int kSteps = 256;
    glm::vec3 sum(0.0f);
    const float pi = std::numbers::pi_v<float>;
    for (int y = 0; y < kSteps; ++y) {
        const float v = (y + 0.5f) / kSteps;
        const float dOmega = (2.0f * pi / (2 * kSteps)) * (pi / kSteps) * std::sin(v * pi);
        for (int x = 0; x < 2 * kSteps; ++x) {
            const glm::vec3 l = equirectDirection((x + 0.5f) / (2 * kSteps), v);
            sum += radiance(l) * std::max(glm::dot(n, l), 0.0f) * dOmega;
        }
    }
    return sum / pi;
}

} // namespace

TEST(Environment, DirectionsRoundTripTheEquirectLayout) {
    EXPECT_NEAR(equirectDirection(0.5f, 0.0f).y, 1.0f, 1e-5f) << "top row is +Y";
    EXPECT_NEAR(equirectDirection(0.5f, 1.0f).y, -1.0f, 1e-5f);
    const glm::vec3 forward = equirectDirection(0.5f, 0.5f);
    EXPECT_NEAR(forward.z, -1.0f, 1e-5f) << "the centre looks down -Z";
    EXPECT_NEAR(equirectDirection(0.75f, 0.5f).x, 1.0f, 1e-5f);
}

TEST(Environment, UniformLightStaysUniform) {
    const auto white = [](glm::vec3) { return glm::vec3(1.0f); };
    const EnvironmentMap env = prefilterEnvironment(makeEnvironment(256, 128, white));
    ASSERT_EQ(env.specular.size(), kEnvironmentSpecularLevels);
    EXPECT_EQ(env.specular[0].width, 256u);
    for (const auto& level : env.specular) {
        for (float c : level.rgb) ASSERT_NEAR(c, 1.0f, 0.02f) << "prefiltering conserves a constant";
    }
    for (glm::vec3 n : {glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::normalize(glm::vec3(1, -1, 1))}) {
        const glm::vec3 e = evaluateIrradiance(env.irradiance, n);
        EXPECT_NEAR(e.x, 1.0f, 0.02f) << "irradiance / pi of a unit sky is 1";
    }
}

TEST(Environment, IrradianceFollowsTheSky) {
    // Bright warm sky above, dim blue ground below.
    const auto sky = [](glm::vec3 d) { return d.y > 0.0f ? glm::vec3(3.0f, 2.0f, 1.0f) : glm::vec3(0.1f, 0.2f, 0.4f); };
    const EnvironmentMap env = prefilterEnvironment(makeEnvironment(512, 256, sky));
    for (glm::vec3 n : {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0), glm::normalize(glm::vec3(0, 1, 1))}) {
        const glm::vec3 sh = evaluateIrradiance(env.irradiance, n);
        const glm::vec3 exact = bruteIrradiance(sky, n);
        // Third-order SH reproduces a cosine-convolved hemisphere step to within ~10 %.
        EXPECT_NEAR(sh.x, exact.x, 0.12f * 3.0f) << "normal " << n.x << "," << n.y << "," << n.z;
        EXPECT_NEAR(sh.z, exact.z, 0.12f * 1.0f);
    }
    EXPECT_GT(evaluateIrradiance(env.irradiance, {0, 1, 0}).x, 2.0f * evaluateIrradiance(env.irradiance, {0, -1, 0}).x);
}

TEST(Environment, RougherLevelsAreBlurrier) {
    // A small bright spot: sharp in level 0, spread out in the roughest level.
    const glm::vec3 spot = glm::normalize(glm::vec3(0.3f, 0.5f, -1.0f));
    const auto light = [&](glm::vec3 d) { return glm::dot(d, spot) > 0.995f ? glm::vec3(100.0f) : glm::vec3(0.0f); };
    const EnvironmentMap env = prefilterEnvironment(makeEnvironment(512, 256, light));
    auto peak = [](const EnvironmentMap::Level& level) {
        float m = 0.0f;
        for (float c : level.rgb) m = std::max(m, c);
        return m;
    };
    auto mean = [](const EnvironmentMap::Level& level) {
        double s = 0.0;
        for (float c : level.rgb) s += c;
        return s / static_cast<double>(level.rgb.size());
    };
    EXPECT_GT(peak(env.specular[0]), 50.0f);
    EXPECT_LT(peak(env.specular.back()), peak(env.specular[2]));
    EXPECT_LT(peak(env.specular[2]), peak(env.specular[0]));
    // Energy is kept roughly (the spot spreads, its total does not vanish or explode).
    EXPECT_NEAR(mean(env.specular.back()) / mean(env.specular[0]), 1.0, 0.5);
}

#include "TestUtils.hpp"
#include "renderer/ReflectionProbes.hpp"

TEST(ReflectionProbes, CubeFacesBecomeTheEquirectDirectionsTheyLookAlong) {
    std::array<HdrImage, 6> faces;
    for (std::size_t f = 0; f < 6; ++f) {
        faces[f].width = faces[f].height = 8;
        faces[f].rgb.assign(8 * 8 * 3, static_cast<float>(f + 1));
    }
    // One face gets a gradient in its "up" direction: brighter towards the top of its image.
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) faces[4].rgb[(y * 8 + x) * 3] = 10.0f + static_cast<float>(7 - y);
    }
    const HdrImage equirect = cubeToEquirect(faces, 64);
    ASSERT_EQ(equirect.width, 64u);
    ASSERT_EQ(equirect.height, 32u);
    auto along = [&](glm::vec3 dir) {
        // Inverse of equirectDirection: u = 0.5 + atan2(x, -z) / 2pi, v = acos(y) / pi.
        const float u = 0.5f + std::atan2(dir.x, -dir.z) / (2.0f * std::numbers::pi_v<float>);
        const float v = std::acos(std::clamp(dir.y, -1.0f, 1.0f)) / std::numbers::pi_v<float>;
        const auto x = std::min(static_cast<std::uint32_t>(u * 64.0f), 63u);
        const auto y = std::min(static_cast<std::uint32_t>(v * 32.0f), 31u);
        return equirect.rgb[(y * 64 + x) * 3 + 1];  // green: the face's constant
    };
    for (std::size_t f = 0; f < 6; ++f) {
        EXPECT_FLOAT_EQ(along(cubeFaces()[f].forward), static_cast<float>(f + 1)) << "face " << f;
    }
    auto red = [&](glm::vec3 dir) {
        const float u = 0.5f + std::atan2(dir.x, -dir.z) / (2.0f * std::numbers::pi_v<float>);
        const float v = std::acos(std::clamp(dir.y, -1.0f, 1.0f)) / std::numbers::pi_v<float>;
        return equirect.rgb[(std::min(static_cast<std::uint32_t>(v * 32.0f), 31u) * 64 + std::min(static_cast<std::uint32_t>(u * 64.0f), 63u)) * 3];
    };
    EXPECT_GT(red(glm::normalize(glm::vec3(0.0f, 0.5f, 1.0f))), red(glm::normalize(glm::vec3(0.0f, -0.5f, 1.0f))))
        << "the +Z face's top is up";
}

TEST(ReflectionProbes, HdrFilesRoundTrip) {
    test::TempDir dir;
    HdrImage image{3, 2, {0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 100.0f, 0.01f, 0.25f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.1f, 0.2f}};
    const auto file = dir.path() / "Saved" / "probe.hdr";
    ASSERT_TRUE(saveHdrFile(file, image).has_value());
    auto loaded = loadHdrFile(file);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    ASSERT_EQ(loaded->width, 3u);
    ASSERT_EQ(loaded->height, 2u);
    for (std::size_t i = 0; i < image.rgb.size(); ++i) {
        // RGBE keeps 8 bits of mantissa relative to the brightest channel.
        const float brightest = std::max({image.rgb[i / 3 * 3], image.rgb[i / 3 * 3 + 1], image.rgb[i / 3 * 3 + 2]});
        EXPECT_NEAR(loaded->rgb[i], image.rgb[i], brightest / 100.0f + 1e-6f) << i;
    }
}

#include "renderer/LightProbes.hpp"

TEST(LightProbes, GridCountsFollowTheSpacingWithinLimits) {
    EXPECT_EQ(probeVolumeCounts({20.0f, 8.0f, 20.0f}, 2.0f), glm::uvec3(10, 4, 10));
    EXPECT_EQ(probeVolumeCounts({0.5f, 0.5f, 0.5f}, 2.0f), glm::uvec3(1, 1, 1));
    const glm::uvec3 many = probeVolumeCounts({1000.0f, 1000.0f, 1000.0f}, 0.5f);
    EXPECT_LE(many.x * many.y * many.z, 8192u);
    EXPECT_LE(many.x, 64u);
}

TEST(LightProbes, VolumesRoundTripThroughTheirFiles) {
    test::TempDir dir;
    ProbeVolumeData data;
    data.counts = {2, 1, 3};
    for (int i = 0; i < 2 * 1 * 3 * 9; ++i) data.sh.emplace_back(static_cast<float>(i), -0.5f * static_cast<float>(i), 0.25f);
    const auto file = dir.path() / "Saved" / "Probes" / "v.lpv";
    ASSERT_TRUE(saveProbeVolume(file, data).has_value());
    auto loaded = loadProbeVolume(file);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->counts, data.counts);
    EXPECT_EQ(loaded->sh, data.sh);
    std::filesystem::resize_file(file, 40);
    EXPECT_FALSE(loadProbeVolume(file).has_value()) << "a truncated file is rejected";
    ProbeVolumeData wrong;
    wrong.counts = {2, 2, 2};
    EXPECT_FALSE(saveProbeVolume(file, wrong).has_value());
}
