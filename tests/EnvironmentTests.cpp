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
