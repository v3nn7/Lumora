// GPU integration tests: render real frames offscreen and inspect the pixels.
// Skipped (not faked) when no Vulkan 1.4 device is available.
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Sprites.hpp"
#include "renderer/VirtualShadowMap.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>

using namespace lumora;
using namespace lumora::render;

namespace {

class RendererTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        RenderContextConfig config;
        config.requirePresentation = false;
        config.enableValidation = std::getenv("LUMORA_TEST_VALIDATION") != nullptr;
        if (const char* gpu = std::getenv("LUMORA_GPU")) config.gpuIndex = static_cast<std::uint32_t>(std::atoi(gpu));
        // LUMORA_BACKEND=d3d12 runs the GPU tests on Direct3D 12 (LUMORA_WARP=1: the software one).
        if (const char* backend = std::getenv("LUMORA_BACKEND")) {
            if (auto b = parseBackend(backend)) config.backend = *b;
        }
        config.warp = std::getenv("LUMORA_WARP") != nullptr;
        try {
            context_ = std::make_unique<RenderContext>(config);
            renderer_ = std::make_unique<Renderer>(*context_);
        } catch (const rhi::UnsupportedHardwareError& e) {
            skipReason_ = e.what();
        }
    }
    static void TearDownTestSuite() {
        renderer_.reset();
        context_.reset();
    }
    void SetUp() override {
        if (!renderer_) GTEST_SKIP() << "no suitable GPU: " << skipReason_;
    }

    static glm::u8vec4 pixel(const CapturedImage& img, std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
        return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
    }

    static std::unique_ptr<Scene> sceneWithSun() {
        auto scene = std::make_unique<Scene>("Test");
        const auto sun = scene->createEntity("Sun");
        scene->registry().get<TransformComponent>(sun).rotationDeg = {-45.0f, 30.0f, 0.0f};
        scene->registry().emplace<LightComponent>(sun).intensity = 3.0f;
        return scene;
    }

    static inline std::unique_ptr<RenderContext> context_;
    static inline std::unique_ptr<Renderer> renderer_;
    static inline std::string skipReason_;
};

} // namespace

TEST_F(RendererTest, RendersLitCubeInFrontOfSky) {
    auto scene = sceneWithSun();
    const auto cube = scene->createEntity("Cube");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(cube);
    mesh.baseColor = {0.9f, 0.05f, 0.05f};
    mesh.roughness = 0.6f;
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 3.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    auto image = renderer_->capture(*scene, cam.view(1.0f), {128, 96}, {});
    ASSERT_TRUE(image.has_value()) << image.error().message;
    ASSERT_EQ(image->rgba.size(), 128u * 96u * 4u);

    const auto center = pixel(*image, 64, 48);
    EXPECT_GT(center.r, 60) << "cube should be lit";
    EXPECT_GT(center.r, center.g + 30);
    EXPECT_GT(center.r, center.b + 30);

    const auto corner = pixel(*image, 2, 2);  // upper sky
    EXPECT_GT(corner.b, corner.r) << "sky should be blue-ish";
    EXPECT_EQ(corner.a, 255);
    EXPECT_EQ(renderer_->stats().scene.visibleObjects, 1u);
}

TEST_F(RendererTest, BackFacesAreCulled) {
    // A plane faces +Y. Seen from below it must be invisible (single-sided, CCW front faces).
    auto scene = sceneWithSun();
    const auto plane = scene->createEntity("Plane");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(plane);
    mesh.mesh = MeshPrimitive::Plane;
    mesh.baseColor = {0.0f, 1.0f, 0.0f};
    mesh.emissive = {0.0f, 1.0f, 0.0f};
    mesh.emissiveStrength = 5.0f;
    scene->registry().get<TransformComponent>(plane).scale = glm::vec3(10.0f);
    scene->updateWorldTransforms();

    FlyCameraController above;
    above.setPosition({0.0f, 2.0f, 0.01f});
    above.lookAt({0.0f, 0.0f, 0.0f});
    auto fromAbove = renderer_->capture(*scene, above.view(1.0f), {64, 64}, {});
    ASSERT_TRUE(fromAbove.has_value());
    const auto top = pixel(*fromAbove, 32, 32);
    EXPECT_GT(top.g, 200) << "front face visible from above";

    FlyCameraController below;
    below.setPosition({0.0f, -2.0f, 0.01f});
    below.lookAt({0.0f, 0.0f, 0.0f});
    auto fromBelow = renderer_->capture(*scene, below.view(1.0f), {64, 64}, {});
    ASSERT_TRUE(fromBelow.has_value());
    const auto bottom = pixel(*fromBelow, 32, 32);
    EXPECT_LT(bottom.g, 200) << "back face must be culled";
}

TEST_F(RendererTest, FrustumCullingSkipsObjectsBehindCamera) {
    auto scene = sceneWithSun();
    for (int i = 0; i < 4; ++i) {
        const auto e = scene->createEntity("Cube");
        scene->registry().emplace<MeshRendererComponent>(e);
        scene->registry().get<TransformComponent>(e).position = {0.0f, 0.0f, i < 2 ? -5.0f - i : 10.0f + i};
    }
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 0.0f});
    cam.setRotation(0.0f, 0.0f);  // looking down -Z
    ASSERT_TRUE(renderer_->capture(*scene, cam.view(1.0f), {32, 32}, {}).has_value());
    EXPECT_EQ(renderer_->stats().scene.visibleObjects, 2u);
    EXPECT_EQ(renderer_->stats().scene.culledObjects, 2u);
}

TEST_F(RendererTest, CaptureRejectsInvalidSize) {
    Scene scene;
    EXPECT_FALSE(renderer_->capture(scene, FlyCameraController{}.view(1.0f), {0, 10}, {}).has_value());
}

#include "GltfTestUtils.hpp"
#include "TestUtils.hpp"
#include "assets/AssetLibrary.hpp"
#include "scene/Particles.hpp"
#include "scene/ModelInstance.hpp"

TEST_F(RendererTest, RendersImportedGltfModel) {
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Models");
    test::writeCubeGltf(project.path() / "Content" / "Models" / "cube.gltf", false);
    AssetLibrary assets(project.path());
    auto model = assets.model(project.path() / "Content" / "Models" / "cube.gltf");
    ASSERT_TRUE(model.has_value()) << model.error().message;

    auto scene = sceneWithSun();
    const entt::entity root = instantiateModel(*scene, **model, "Content/Models/cube.gltf", "Model");
    scene->registry().get<TransformComponent>(root).position = {0.0f, -1.0f, 0.0f};  // node lifts it by 1
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 4.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    auto image = renderer_->capture(*scene, cam.view(1.0f), {96, 96}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;
    const auto center = pixel(*image, 48, 48);
    EXPECT_GT(center.r, 60);
    EXPECT_GT(center.r, center.g + 30) << "imported red material";
    EXPECT_EQ(renderer_->stats().scene.visibleObjects, 1u);

    // Without an asset library the reference cannot be resolved and nothing is drawn.
    ASSERT_TRUE(renderer_->capture(*scene, cam.view(1.0f), {32, 32}, {}).has_value());
    EXPECT_EQ(renderer_->stats().scene.visibleObjects, 0u);
}

TEST_F(RendererTest, SamplesMaterialTextures) {
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Models");
    test::writeCubeGltf(project.path() / "Content" / "Models" / "textured.gltf", false, true);
    AssetLibrary assets(project.path());
    auto model = assets.model(project.path() / "Content" / "Models" / "textured.gltf");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    auto scene = sceneWithSun();
    const entt::entity root = instantiateModel(*scene, **model, "Content/Models/textured.gltf", "Model");
    scene->registry().get<TransformComponent>(root).position = {0.0f, -1.0f, 0.0f};
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 4.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    auto image = renderer_->capture(*scene, cam.view(1.0f), {96, 96}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;
    const auto center = pixel(*image, 48, 48);
    EXPECT_GT(center.g, center.r + 40) << "white material * green texture renders green";
    EXPECT_GT(center.g, center.b + 40);
}

// ---- Sun shadow cascades --------------------------------------------------------------------

#include "renderer/SceneRenderer.hpp"

#include <glm/gtc/matrix_transform.hpp>

TEST(ShadowCascades, SplitsIncreaseAndEndAtTheShadowDistance) {
    const auto practical = cascadeSplits(0.05f, 50.0f, 0.8f);
    float previous = 0.05f;
    for (float split : practical) {
        EXPECT_GT(split, previous);
        previous = split;
    }
    EXPECT_FLOAT_EQ(practical.back(), 50.0f);
    EXPECT_LT(practical.front(), 50.0f / kShadowCascades) << "logarithmic part favours the near cascade";

    const auto uniform = cascadeSplits(0.0f, 40.0f, 0.0f);  // near clamps to 1 cm
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        EXPECT_NEAR(uniform[i], 0.01f + (40.0f - 0.01f) * static_cast<float>(i + 1) / kShadowCascades, 1e-3f);
    }
}

TEST(ShadowCascades, CascadeBoxContainsItsViewSlice) {
    FlyCameraController cam;
    cam.setPosition({3.0f, 2.0f, 7.0f});
    cam.setRotation(35.0f, -20.0f);
    const CameraView view = cam.view(16.0f / 9.0f);
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, -0.6f, -0.5f));
    const float distance = 12.0f;
    const ShadowFit fit = fitSunShadow(view, lightDir, distance, 1024);

    const glm::mat4 invView = glm::inverse(view.view);
    const glm::vec3 right(invView[0]), up(invView[1]), forward = -glm::vec3(invView[2]);
    const float tanHalf = std::tan(view.fovYRadians * 0.5f);
    glm::vec3 centroid(0.0f);
    for (float d : {view.nearPlane, distance}) {
        for (float sx : {-1.0f, 1.0f}) {
            for (float sy : {-1.0f, 1.0f}) {
                const glm::vec3 corner = view.position + forward * d + right * (d * tanHalf * view.aspect * sx) +
                                         up * (d * tanHalf * sy);
                centroid += corner / 8.0f;
                const glm::vec4 clip = fit.viewProjection * glm::vec4(corner, 1.0f);
                EXPECT_LE(std::abs(clip.x), 1.0f + 1e-3f);
                EXPECT_LE(std::abs(clip.y), 1.0f + 1e-3f);
                EXPECT_GT(clip.z, 0.0f);
                EXPECT_LT(clip.z, 1.0f);
            }
        }
    }
    // The shader blends towards the next cascade by depth around 0.5 (the slice centre).
    EXPECT_NEAR((fit.viewProjection * glm::vec4(centroid, 1.0f)).z, 0.5f, 1e-3f);
}

namespace {

/// Pixel a world point lands on (reversed-Z projection with Vulkan's +Y-down NDC).
glm::ivec2 toPixel(const CameraView& view, glm::vec3 world, glm::uvec2 size) {
    const glm::vec4 clip = view.projection * view.view * glm::vec4(world, 1.0f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return {static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(size.x)),
            static_cast<int>((ndc.y * 0.5f + 0.5f) * static_cast<float>(size.y))};
}

/// Ground plane, a sun travelling down and towards -Z, fog off.
std::unique_ptr<Scene> shadowScene() {
    auto scene = std::make_unique<Scene>("Shadows");
    scene->environment().fogDensity = 0.0f;
    const auto sun = scene->createEntity("Sun");
    scene->registry().get<TransformComponent>(sun).rotationDeg = {-60.0f, 0.0f, 0.0f};  // (0, -0.87, -0.5)
    scene->registry().emplace<LightComponent>(sun).intensity = 3.0f;
    const auto ground = scene->createEntity("Ground");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(ground);
    mesh.mesh = MeshPrimitive::Plane;
    mesh.baseColor = glm::vec3(0.8f);
    scene->registry().get<TransformComponent>(ground).scale = glm::vec3(400.0f, 1.0f, 400.0f);
    return scene;
}

entt::entity addCube(Scene& scene, glm::vec3 position) {
    const auto cube = scene.createEntity("Cube");
    scene.registry().emplace<MeshRendererComponent>(cube);
    scene.registry().get<TransformComponent>(cube).position = position;
    return cube;
}

float luminance(glm::u8vec4 p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; }

} // namespace

TEST_F(RendererTest, SunShadowsNearAndFar) {
    // A cube floating 2 m up casts its shadow 2 / tan(60) = 1.15 m towards -Z.
    auto scene = shadowScene();
    for (glm::vec3 p : {glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 2.0f, -36.0f)}) addCube(*scene, p);
    scene->updateWorldTransforms();
    const glm::uvec2 size(160, 120);
    const glm::vec3 shadowOffset(0.0f, -2.0f, -2.0f / std::tan(glm::radians(60.0f)));

    FlyCameraController cam;
    cam.setPosition({4.0f, 3.0f, 4.0f});
    cam.lookAt({0.0f, 0.0f, -2.0f});
    const CameraView view = cam.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, {});
    ASSERT_TRUE(image.has_value()) << image.error().message;
    ASSERT_TRUE(renderer_->stats().scene.shadowsRendered);
    for (glm::vec3 caster : {glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 2.0f, -36.0f)}) {
        const glm::ivec2 shadow = toPixel(view, caster + shadowOffset, size);
        const glm::ivec2 lit = toPixel(view, caster + glm::vec3(2.5f, -2.0f, 0.0f), size);  // beside the shadow
        ASSERT_TRUE(shadow.x >= 0 && shadow.y >= 0 && shadow.x < 160 && shadow.y < 120);
        ASSERT_TRUE(lit.x >= 0 && lit.y >= 0 && lit.x < 160 && lit.y < 120);
        const float inShadow = luminance(pixel(*image, shadow.x, shadow.y));
        const float inLight = luminance(pixel(*image, lit.x, lit.y));
        EXPECT_LT(inShadow, inLight * 0.6f) << "caster at z=" << caster.z << ": shadow " << inShadow << " vs lit "
                                             << inLight;
    }

    RenderSettings off;
    off.shadows = false;
    auto unshadowed = renderer_->capture(*scene, view, {size.x, size.y}, off);
    ASSERT_TRUE(unshadowed.has_value());
    const glm::ivec2 shadow = toPixel(view, glm::vec3(0.0f, 2.0f, 0.0f) + shadowOffset, size);
    EXPECT_GT(luminance(pixel(*unshadowed, shadow.x, shadow.y)), luminance(pixel(*image, shadow.x, shadow.y)) * 1.5f);
}

TEST_F(RendererTest, OffscreenCastersStillCastShadows) {
    // The caster floats above and behind the camera (culled from the view), but its shadow
    // lands in front of it.
    auto scene = shadowScene();
    const float height = 12.0f;
    const float reach = height / std::tan(glm::radians(60.0f));  // shadow travels this far towards -Z
    addCube(*scene, {0.0f, height, reach});
    scene->updateWorldTransforms();
    const glm::uvec2 size(96, 96);
    FlyCameraController cam;
    cam.setPosition({0.0f, 4.0f, 2.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, {});
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(renderer_->stats().scene.culledObjects, 1u) << "the caster itself is not visible";
    const glm::ivec2 shadow = toPixel(view, {0.0f, 0.0f, 0.0f}, size);
    const glm::ivec2 lit = toPixel(view, {2.0f, 0.0f, 0.0f}, size);
    EXPECT_LT(luminance(pixel(*image, shadow.x, shadow.y)), luminance(pixel(*image, lit.x, lit.y)) * 0.6f);
}

TEST_F(RendererTest, VirtualShadowMapsCastLikeCascadesAndKeepTheirPages) {
    auto scene = shadowScene();
    const entt::entity cube = addCube(*scene, {0.0f, 2.0f, 0.0f});
    scene->updateWorldTransforms();
    const glm::uvec2 size(160, 120);
    const glm::vec3 shadowOffset(0.0f, -2.0f, -2.0f / std::tan(glm::radians(60.0f)));
    FlyCameraController cam;
    cam.setPosition({4.0f, 3.0f, 4.0f});
    cam.lookAt({0.0f, 0.0f, -2.0f});
    const CameraView view = cam.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    auto darkness = [&](const CapturedImage& image, glm::vec3 caster) {
        const glm::ivec2 shadow = toPixel(view, caster + shadowOffset, size);
        const glm::ivec2 lit = toPixel(view, caster + glm::vec3(2.5f, -2.0f, 0.0f), size);
        return luminance(pixel(image, shadow.x, shadow.y)) / std::max(luminance(pixel(image, lit.x, lit.y)), 1.0f);
    };

    RenderSettings cascades;
    cascades.virtualShadows = false;
    auto classic = renderer_->capture(*scene, view, {size.x, size.y}, cascades);
    ASSERT_TRUE(classic.has_value());
    EXPECT_FALSE(renderer_->stats().scene.virtualShadows);

    RenderSettings all;
    all.virtualShadowPageBudget = 0;  // every page in one frame (the default spreads them out)
    auto first = renderer_->capture(*scene, view, {size.x, size.y}, all);
    ASSERT_TRUE(first.has_value());
    const SceneDrawStats firstStats = renderer_->stats().scene;
    EXPECT_TRUE(firstStats.virtualShadows);
    EXPECT_GT(firstStats.virtualPagesRequested, 0u);
    EXPECT_EQ(firstStats.virtualPagesRendered + firstStats.virtualPagesCached, firstStats.virtualPagesRequested)
        << "every page drawn (or still cached from an earlier capture of the same scene)";
    EXPECT_LT(darkness(*first, {0.0f, 2.0f, 0.0f}), 0.6f) << "the cube's shadow";
    EXPECT_NEAR(darkness(*first, {0.0f, 2.0f, 0.0f}), darkness(*classic, {0.0f, 2.0f, 0.0f}), 0.15f) << "as dark as with cascades";

    // Nothing changed: every page comes from the cache.
    auto again = renderer_->capture(*scene, view, {size.x, size.y}, {});
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(renderer_->stats().scene.virtualPagesRendered, 0u);
    EXPECT_EQ(renderer_->stats().scene.virtualPagesCached, renderer_->stats().scene.virtualPagesRequested);
    EXPECT_EQ(glm::ivec3(pixel(*again, 80, 60)), glm::ivec3(pixel(*first, 80, 60)));

    // The cube moves: only the pages under it are drawn again, and the shadow follows.
    scene->registry().get<TransformComponent>(cube).position = {1.5f, 2.0f, 0.0f};
    scene->updateWorldTransforms();
    auto moved = renderer_->capture(*scene, view, {size.x, size.y}, {});
    ASSERT_TRUE(moved.has_value());
    const SceneDrawStats movedStats = renderer_->stats().scene;
    EXPECT_GT(movedStats.virtualPagesRendered, 0u);
    EXPECT_LT(movedStats.virtualPagesRendered, movedStats.virtualPagesRequested) << "the rest stay cached";
    EXPECT_LT(darkness(*moved, {1.5f, 2.0f, 0.0f}), 0.6f);
    const glm::ivec2 oldShadow = toPixel(view, glm::vec3(0.0f, 2.0f, 0.0f) + shadowOffset, size);
    const glm::ivec2 newLit = toPixel(view, glm::vec3(0.0f, 2.0f, 0.0f) + glm::vec3(-2.5f, -2.0f, 0.0f), size);
    EXPECT_GT(luminance(pixel(*moved, oldShadow.x, oldShadow.y)), 0.75f * luminance(pixel(*moved, newLit.x, newLit.y)))
        << "where the shadow was is lit again";
}

TEST(VirtualShadowMap, RequestsCachesInvalidatesAndFallsBack) {
    VirtualShadowMap vsm;
    FlyCameraController cam;
    cam.setPosition({0.0f, 2.0f, 0.0f});
    cam.setRotation(0.0f, -20.0f);
    const CameraView view = cam.view(16.0f / 9.0f);
    const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f));
    std::vector<VsmCaster> casters{{1, {0.0f, 1.0f, -5.0f}, glm::vec3(0.5f), 7, false},
                                   {2, {30.0f, 1.0f, -40.0f}, glm::vec3(0.5f), 9, false}};

    VsmFrame f = vsm.update(view, sun, 50.0f, casters, 0);
    EXPECT_TRUE(f.invalidatedAll);
    EXPECT_GE(f.levels, 4);
    EXPECT_GT(f.requested, 20u);
    EXPECT_EQ(f.render.size(), f.requested) << "everything is drawn the first time";
    EXPECT_LT(f.requested, static_cast<std::uint32_t>(VirtualShadowMap::kPoolPages * VirtualShadowMap::kPoolPages))
        << "far fewer pages than the levels hold: only those under the view";
    for (std::size_t i = 1; i < f.render.size(); ++i) EXPECT_GE(f.render[i - 1].level, f.render[i].level) << "coarse levels first";
    int valid = 0;
    for (std::size_t i = 3; i < f.pageTable.size(); i += 4) valid += f.pageTable[i] > 0.5f ? 1 : 0;
    EXPECT_EQ(valid, static_cast<int>(f.requested));

    f = vsm.update(view, sun, 50.0f, casters, 0);
    EXPECT_FALSE(f.invalidatedAll);
    EXPECT_TRUE(f.render.empty());
    EXPECT_EQ(f.cached, f.requested);

    // One caster moves: the pages under its old and new place, nothing else.
    casters[0].center.x += 0.3f;
    casters[0].state = 8;
    f = vsm.update(view, sun, 50.0f, casters, 0);
    EXPECT_FALSE(f.render.empty());
    EXPECT_LT(f.render.size(), f.requested / 2);
    // Animated casters redraw every frame.
    casters[1].dynamic = true;
    f = vsm.update(view, sun, 50.0f, casters, 0);
    const std::size_t dynamicPages = f.render.size();
    f = vsm.update(view, sun, 50.0f, casters, 0);
    EXPECT_EQ(f.render.size(), dynamicPages);
    casters[1].dynamic = false;

    // The sun turns: every page is stale; a budget draws the coarse ones first.
    f = vsm.update(view, glm::normalize(sun + glm::vec3(0.1f, 0.0f, 0.0f)), 50.0f, casters, 16);
    EXPECT_TRUE(f.invalidatedAll);
    ASSERT_EQ(f.render.size(), 16u);
    EXPECT_EQ(f.render.front().level, f.levels - 1);
    f = vsm.update(view, glm::normalize(sun + glm::vec3(0.1f, 0.0f, 0.0f)), 50.0f, casters, 0);
    EXPECT_EQ(f.render.size() + 16u, f.requested) << "the rest follow in the next frame";
}

TEST_F(RendererTest, CascadeDebugViewShowsFinerCascadesNearTheCamera) {
    auto scene = shadowScene();
    scene->updateWorldTransforms();
    const glm::uvec2 size(96, 96);
    FlyCameraController cam;
    cam.setPosition({0.0f, 1.5f, 0.0f});
    cam.setRotation(0.0f, -10.0f);  // looking down -Z, slightly down
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.debugCascades = true;
    settings.virtualShadows = false;  // the four cascades
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value());
    const glm::u8vec4 nearPixel = pixel(*image, toPixel(view, {0.0f, 0.0f, -3.0f}, size).x, toPixel(view, {0.0f, 0.0f, -3.0f}, size).y);
    const glm::ivec2 farPx = toPixel(view, {0.0f, 0.0f, -40.0f}, size);
    const glm::u8vec4 farPixel = pixel(*image, farPx.x, farPx.y);
    EXPECT_GT(nearPixel.r, nearPixel.g + 40) << "cascade 0 is tinted red";
    EXPECT_GT(farPixel.g, farPixel.b + 40) << "cascade 3 is tinted yellow";
    EXPECT_GT(farPixel.r, farPixel.b + 40);
}

TEST_F(RendererTest, ParallelRecordingMatchesSerial) {
    // A grid of casters over a ground plane, recorded once on one thread and once with the
    // shadow and opaque passes split into secondary command buffers on the job system: the
    // images must be identical, pixel for pixel.
    auto scene = shadowScene();
    for (int x = -6; x <= 6; ++x) {
        for (int z = -6; z <= 6; ++z) addCube(*scene, glm::vec3(x * 1.6f, 1.0f + 0.1f * ((x + z) & 3), z * 1.6f));
    }
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({9.0f, 7.0f, 11.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(4.0f / 3.0f);

    RenderSettings serial;
    serial.parallelRecordingMinDraws = UINT32_MAX;
    auto one = renderer_->capture(*scene, view, {160, 120}, serial);
    ASSERT_TRUE(one.has_value()) << one.error().message;
    EXPECT_FALSE(renderer_->stats().scene.parallelRecording);

    RenderSettings parallel;
    parallel.parallelRecordingMinDraws = 0;
    auto many = renderer_->capture(*scene, view, {160, 120}, parallel);
    ASSERT_TRUE(many.has_value()) << many.error().message;
    EXPECT_TRUE(renderer_->stats().scene.parallelRecording);
    ASSERT_TRUE(renderer_->stats().scene.shadowsRendered);
    ASSERT_EQ(one->rgba.size(), many->rgba.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < one->rgba.size(); ++i) differing += one->rgba[i] != many->rgba[i] ? 1 : 0;
    EXPECT_EQ(differing, 0u) << "parallel recording must not change the image";
}

// ---- Image-based lighting -------------------------------------------------------------------

#include <stb_image_write.h>

TEST_F(RendererTest, HdrEnvironmentLightsTheSceneAndDrawsTheSky) {
    // Red sky above, blue ground below, no lights: a rough white sphere is lit only by the
    // environment (reddish on top, bluish underneath) and the background shows the HDR.
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Textures");
    constexpr int kW = 128, kH = 64;
    std::vector<float> rgb(kW * kH * 3, 0.0f);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            float* p = &rgb[(y * kW + x) * 3];
            if (y < kH / 2) {
                p[0] = 2.0f;
            } else {
                p[2] = 2.0f;
            }
        }
    }
    const auto file = project.path() / "Content" / "Textures" / "split.hdr";
    ASSERT_NE(stbi_write_hdr(file.string().c_str(), kW, kH, 3, rgb.data()), 0);

    auto scene = std::make_unique<Scene>("Env");
    const entt::entity ball = scene->createEntity("Ball");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(ball);
    mesh.mesh = MeshPrimitive::Sphere;
    mesh.baseColor = glm::vec3(0.9f);
    mesh.roughness = 1.0f;
    scene->environment().skyTexture = "Content/Textures/split.hdr";
    scene->environment().ambientIntensity = 1.0f;
    scene->environment().fogDensity = 0.0f;
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 3.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    AssetLibrary assets(project.path());
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.bloom = false;
    auto image = renderer_->capture(*scene, cam.view(1.0f), {96, 96}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;
    EXPECT_TRUE(renderer_->stats().scene.environmentLit);

    const auto top = pixel(*image, 48, 36);     // upper part of the sphere
    const auto bottom = pixel(*image, 48, 60);  // lower part
    EXPECT_GT(top.r, top.b + 30) << "the upper hemisphere sees the red sky";
    EXPECT_GT(bottom.b, bottom.r + 30) << "the lower hemisphere sees the blue ground";
    const auto skyAbove = pixel(*image, 48, 2);
    const auto skyBelow = pixel(*image, 48, 93);
    EXPECT_GT(skyAbove.r, 150);
    EXPECT_LT(skyAbove.b, 40);
    EXPECT_GT(skyBelow.b, 150);
    EXPECT_LT(skyBelow.r, 40);
}

TEST_F(RendererTest, AmbientOcclusionDarkensContactsOnly) {
    // Ambient light only (no sun): a cube resting on the ground. With SSAO the ground right at
    // the cube's base gets darker; open ground far from it stays as it was.
    auto scene = shadowScene();
    scene->destroyEntity(scene->registry().view<LightComponent>().front());
    scene->environment().ambientIntensity = 2.0f;
    const auto box = scene->createEntity("Box");
    scene->registry().emplace<MeshRendererComponent>(box).baseColor = glm::vec3(0.8f);
    scene->registry().get<TransformComponent>(box).position = glm::vec3(0.0f, 0.5f, 0.0f);
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({2.5f, 1.6f, 2.5f});
    cam.lookAt({0.0f, 0.2f, 0.0f});
    const glm::uvec2 size(160, 120);
    const CameraView view = cam.view(static_cast<float>(size.x) / size.y);
    RenderSettings off;
    off.ambientOcclusion = false;
    off.bloom = false;
    RenderSettings on = off;
    on.ambientOcclusion = true;
    auto plain = renderer_->capture(*scene, view, {size.x, size.y}, off);
    ASSERT_TRUE(plain.has_value()) << plain.error().message;
    EXPECT_FALSE(renderer_->stats().scene.ambientOcclusion);
    auto occluded = renderer_->capture(*scene, view, {size.x, size.y}, on);
    ASSERT_TRUE(occluded.has_value()) << occluded.error().message;
    EXPECT_TRUE(renderer_->stats().scene.ambientOcclusion);

    const glm::ivec2 contact = toPixel(view, glm::vec3(0.6f, 0.0f, 0.2f), size);  // ground at the cube's foot
    const glm::ivec2 open = toPixel(view, glm::vec3(2.0f, 0.0f, -1.5f), size);   // ground far from it
    ASSERT_TRUE(contact.x >= 0 && contact.y >= 0 && contact.x < 160 && contact.y < 120);
    ASSERT_TRUE(open.x >= 0 && open.y >= 0 && open.x < 160 && open.y < 120);
    EXPECT_LT(luminance(pixel(*occluded, contact.x, contact.y)), luminance(pixel(*plain, contact.x, contact.y)) * 0.92f)
        << "the crease where cube meets ground is occluded";
    EXPECT_NEAR(luminance(pixel(*occluded, open.x, open.y)), luminance(pixel(*plain, open.x, open.y)), 6.0f)
        << "flat open ground is not darkened";
}

TEST_F(RendererTest, FxaaSmoothsEdgesOnly) {
    // A tilted cube against the sky: FXAA changes pixels along its silhouette, nothing else.
    auto scene = sceneWithSun();
    const auto cube = scene->createEntity("Cube");
    scene->registry().emplace<MeshRendererComponent>(cube).baseColor = glm::vec3(0.9f);
    scene->registry().get<TransformComponent>(cube).rotationDeg = glm::vec3(20.0f, 33.0f, 7.0f);
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 3.5f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    RenderSettings plain;
    plain.antiAliasing = false;
    plain.bloom = false;
    RenderSettings smooth = plain;
    smooth.antiAliasing = true;
    auto a = renderer_->capture(*scene, cam.view(1.0f), {128, 128}, plain);
    auto b = renderer_->capture(*scene, cam.view(1.0f), {128, 128}, smooth);
    ASSERT_TRUE(a.has_value() && b.has_value());
    std::size_t changed = 0;
    std::size_t changedAwayFromEdges = 0;
    for (std::uint32_t y = 1; y + 1 < 128; ++y) {
        for (std::uint32_t x = 1; x + 1 < 128; ++x) {
            // Both paths dither and quantise slightly differently: count real changes only.
            if (std::abs(luminance(pixel(*b, x, y)) - luminance(pixel(*a, x, y))) < 3.0f) continue;
            ++changed;
            // An edge pixel has a neighbour (in the unfiltered image) with clearly different luma.
            float contrast = 0.0f;
            for (glm::ivec2 d : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)}) {
                contrast = std::max(contrast, std::abs(luminance(pixel(*a, x + d.x, y + d.y)) - luminance(pixel(*a, x, y))));
            }
            if (contrast < 4.0f) ++changedAwayFromEdges;
        }
    }
    EXPECT_GT(changed, 40u) << "the silhouette got smoothed";
    EXPECT_LT(changed, 128u * 128u / 8u) << "FXAA only touches edges";
    EXPECT_LT(changedAwayFromEdges, changed / 10 + 2) << "flat areas stay as they were";
}

// ---- Skinning -------------------------------------------------------------------------------

#include "GltfTestUtils.hpp"

TEST_F(RendererTest, SkinnedMeshFollowsItsJoints) {
    test::TempDir project;
    test::writeSkinnedGltf(project.path() / "Content" / "Models" / "bend.gltf");
    AssetLibrary assets(project.path());
    auto model = assets.model(project.path() / "Content" / "Models" / "bend.gltf");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    ASSERT_EQ((*model)->skins.size(), 1u);
    ASSERT_EQ((*model)->meshes[0].data.skin.size(), 6u);

    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const entt::entity root = instantiateModel(*scene, **model, "Content/Models/bend.gltf", "Bender");
    (void)root;
    entt::entity bone = entt::null, body = entt::null;
    for (auto [e, n] : scene->registry().view<NameComponent>().each()) {
        if (n.name == "Bone") bone = e;
        if (n.name == "Body") body = e;
    }
    ASSERT_TRUE(bone != entt::null && body != entt::null);
    EXPECT_TRUE(scene->registry().all_of<SkinComponent>(body));
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 1.0f, 5.0f});
    cam.lookAt({0.0f, 1.0f, 0.0f});
    const glm::uvec2 size(96, 96);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.bloom = false;
    auto isRed = [](glm::u8vec4 p) { return p.r > 2 * p.g + 20 && p.r > 2 * p.b + 20; };

    auto straight = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(straight.has_value()) << straight.error().message;
    const glm::ivec2 top = toPixel(view, glm::vec3(0.0f, 1.8f, 0.0f), size);
    const glm::ivec2 left = toPixel(view, glm::vec3(-0.85f, 1.2f, 0.0f), size);
    EXPECT_TRUE(isRed(pixel(*straight, top.x, top.y))) << "upright: the quad covers (0, 1.8)";
    EXPECT_FALSE(isRed(pixel(*straight, left.x, left.y)));

    // Bend: Bone turns 90 degrees about +Z, so the top half swings to the left (x < 0).
    scene->registry().get<TransformComponent>(bone).rotationDeg = glm::vec3(0.0f, 0.0f, 90.0f);
    scene->updateWorldTransforms();
    auto bent = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(bent.has_value());
    EXPECT_FALSE(isRed(pixel(*bent, top.x, top.y))) << "the top left (0, 1.8)";
    EXPECT_TRUE(isRed(pixel(*bent, left.x, left.y))) << "and now lies at (-0.85, 1.2)";
}

// ---- Point lights (tiled) -------------------------------------------------------------------

#include "renderer/LightGrid.hpp"

TEST(LightGrid, BinsLightsByTheTilesTheyCover) {
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 10.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    auto light = [](glm::vec3 p, float range) {
        GpuLight l;
        l.positionRange = glm::vec4(p, range);
        return l;
    };
    const std::vector<GpuLight> lights = {
        light({0, 0, 0}, 1.0f),     // small, in the middle
        light({0, 0, 20}, 2.0f),    // behind the camera
        light({0, 0, 10}, 3.0f),    // the camera is inside it
        light({500, 0, 0}, 1.0f),   // far off to the side
    };
    const LightGrid grid = buildLightGrid(lights, view.view, view.projection, 256, 256, 32);
    ASSERT_EQ(grid.tilesX, 8u);
    ASSERT_EQ(grid.tilesY, 8u);
    auto has = [&](std::uint32_t x, std::uint32_t y, std::uint32_t lightIndex) {
        const std::uint32_t t = y * grid.tilesX + x;
        for (std::uint32_t k = 0; k < grid.data[t * 2 + 1]; ++k) {
            if (grid.data[grid.data[t * 2] + k] == lightIndex) return true;
        }
        return false;
    };
    EXPECT_TRUE(has(4, 4, 0)) << "the centre light covers the centre tiles";
    EXPECT_FALSE(has(0, 0, 0)) << "but not the corners";
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            EXPECT_FALSE(has(x, y, 1));
            EXPECT_TRUE(has(x, y, 2)) << "inside a light: every tile";
            EXPECT_FALSE(has(x, y, 3));
        }
    }
}

TEST_F(RendererTest, ManyPointLightsLightTheirSurroundings) {
    // 120 dim green lights over a dark floor, then one strong red light (number 121, far past the
    // old limit of 16) above one spot: that spot turns red, another spot only sees green.
    auto scene = std::make_unique<Scene>("Lights");
    scene->environment().ambientIntensity = 0.0f;
    scene->environment().fogDensity = 0.0f;
    const auto ground = scene->createEntity("Ground");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(ground);
    mesh.mesh = MeshPrimitive::Plane;
    mesh.baseColor = glm::vec3(0.8f);
    mesh.roughness = 1.0f;
    scene->registry().get<TransformComponent>(ground).scale = glm::vec3(60.0f, 1.0f, 60.0f);
    for (int i = 0; i < 120; ++i) {
        const auto e = scene->createEntity("Green");
        scene->registry().get<TransformComponent>(e).position = glm::vec3((i % 12) * 2.0f - 11.0f, 0.6f, (i / 12) * 2.0f - 9.0f);
        auto& l = scene->registry().emplace<LightComponent>(e);
        l.type = LightType::Point;
        l.color = glm::vec3(0.0f, 1.0f, 0.0f);
        l.intensity = 1.0f;
        l.range = 1.5f;
    }
    const auto red = scene->createEntity("Red");
    scene->registry().get<TransformComponent>(red).position = glm::vec3(4.0f, 0.5f, 4.0f);
    auto& r = scene->registry().emplace<LightComponent>(red);
    r.type = LightType::Point;
    r.color = glm::vec3(1.0f, 0.0f, 0.0f);
    r.intensity = 6.0f;
    r.range = 1.5f;
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 14.0f, 0.1f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(160, 160);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;
    EXPECT_EQ(renderer_->stats().scene.lights, 121u);
    const glm::ivec2 underRed = toPixel(view, glm::vec3(4.0f, 0.0f, 4.0f), size);
    const glm::ivec2 underGreen = toPixel(view, glm::vec3(-5.0f, 0.0f, -3.0f), size);
    const auto a = pixel(*image, underRed.x, underRed.y);
    const auto b = pixel(*image, underGreen.x, underGreen.y);
    EXPECT_GT(a.r, a.g + 40) << "light 121 reaches the floor below it";
    EXPECT_GT(b.g, b.r + 20) << "the green lights light theirs";
    EXPECT_LT(b.r, 30);
}

TEST_F(RendererTest, SpotLightLightsOnlyItsCone) {
    // A spot light 3 m above a dark floor pointing down with a 20 degree cone: the floor below it
    // is lit, the floor 2.5 m to the side (well inside its range) stays dark.
    auto scene = std::make_unique<Scene>("Spot");
    scene->environment().ambientIntensity = 0.0f;
    scene->environment().fogDensity = 0.0f;
    const auto ground = scene->createEntity("Ground");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(ground);
    mesh.mesh = MeshPrimitive::Plane;
    mesh.baseColor = glm::vec3(0.8f);
    mesh.roughness = 1.0f;
    scene->registry().get<TransformComponent>(ground).scale = glm::vec3(20.0f, 1.0f, 20.0f);
    const auto spot = scene->createEntity("Spot");
    auto& t = scene->registry().get<TransformComponent>(spot);
    t.position = glm::vec3(0.0f, 3.0f, 0.0f);
    t.rotationDeg = glm::vec3(-90.0f, 0.0f, 0.0f);  // -Z turned to point down
    auto& l = scene->registry().emplace<LightComponent>(spot);
    l.type = LightType::Spot;
    l.intensity = 30.0f;
    l.range = 12.0f;
    l.spotAngleDeg = 20.0f;
    l.spotSoftness = 0.2f;
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 10.0f, 0.1f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(128, 128);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;
    const glm::ivec2 centre = toPixel(view, glm::vec3(0.0f, 0.0f, 0.0f), size);
    const glm::ivec2 side = toPixel(view, glm::vec3(2.5f, 0.0f, 0.0f), size);
    const auto lit = pixel(*image, centre.x, centre.y);
    const auto dark = pixel(*image, side.x, side.y);
    EXPECT_GT(lit.r, 120) << "inside the cone";
    EXPECT_LT(dark.r, 15) << "outside the cone (a point light this strong would light it)";
}

TEST_F(RendererTest, MaterialAssetsColourEveryObjectUsingThem) {
    // Two cubes share Content/Materials/Paint.lmat (red), their own colour is grey; editing
    // the file turns both blue.
    test::TempDir project;
    MaterialAsset paint;
    paint.baseColor = {1.0f, 0.0f, 0.0f};
    paint.roughness = 1.0f;
    const auto file = project.path() / "Content" / "Materials" / "Paint.lmat";
    ASSERT_TRUE(saveMaterial(paint, file).has_value());
    AssetLibrary assets(project.path());

    auto scene = sceneWithSun();
    for (float x : {-1.2f, 1.2f}) {
        const auto e = scene->createEntity("Cube");
        auto& mesh = scene->registry().emplace<MeshRendererComponent>(e);
        mesh.baseColor = glm::vec3(0.5f);
        mesh.material = "Content/Materials/Paint.lmat";
        scene->registry().get<TransformComponent>(e).position = {x, 0.0f, 0.0f};
    }
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 1.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(128, 96);
    const CameraView view = cam.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.bloom = false;
    const glm::ivec2 left = toPixel(view, glm::vec3(-1.2f, 0.0f, 0.5f), size);
    const glm::ivec2 right = toPixel(view, glm::vec3(1.2f, 0.0f, 0.5f), size);

    auto red = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(red.has_value()) << red.error().message;
    for (const glm::ivec2 p : {left, right}) {
        const auto c = pixel(*red, p.x, p.y);
        EXPECT_GT(c.r, c.g + 40) << "red from the material, not the grey of the component";
        EXPECT_GT(c.r, c.b + 40);
    }

    paint.baseColor = {0.0f, 0.0f, 1.0f};
    ASSERT_TRUE(saveMaterial(paint, file).has_value());
    assets.reloadMaterial("Content/Materials/Paint.lmat");
    auto blue = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(blue.has_value());
    for (const glm::ivec2 p : {left, right}) {
        const auto c = pixel(*blue, p.x, p.y);
        EXPECT_GT(c.b, c.r + 40) << "both cubes follow the edited file";
    }

    // A missing material falls back to the component's own settings.
    std::filesystem::remove(file);
    assets.reloadMaterial("Content/Materials/Paint.lmat");
    auto grey = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(grey.has_value());
    const auto g = pixel(*grey, left.x, left.y);
    EXPECT_LT(std::abs(int(g.r) - int(g.b)), 20) << "grey again";
}

TEST_F(RendererTest, ParticlesGlowBlendAndHideBehindGeometry) {
    // Dark scene; a cloud of additive orange particles on the left lights the image there; the
    // same cloud on the right sits behind a black wall and does not show.
    auto scene = std::make_unique<Scene>("Particles");
    scene->environment().ambientIntensity = 0.0f;
    scene->environment().fogDensity = 0.0f;
    scene->environment().skyZenithColor = glm::vec3(0.0f);
    scene->environment().skyHorizonColor = glm::vec3(0.0f);
    scene->environment().groundColor = glm::vec3(0.0f);
    ParticleEmitterComponent sparks;
    sparks.playing = false;
    sparks.burst = 200;
    sparks.maxParticles = 200;
    sparks.speed = 0.0f;
    sparks.shape = ParticleShape::Sphere;
    sparks.shapeSize = 0.4f;
    sparks.lifetime = 100.0f;
    sparks.startSize = sparks.endSize = 0.3f;
    sparks.startColor = sparks.endColor = {1.0f, 0.5f, 0.1f};
    sparks.startAlpha = sparks.endAlpha = 1.0f;
    sparks.intensity = 2.0f;
    sparks.blend = ParticleBlend::Additive;
    for (float x : {-1.5f, 1.5f}) {
        const auto e = scene->createEntity("Sparks");
        scene->registry().emplace<ParticleEmitterComponent>(e, sparks);
        scene->registry().get<TransformComponent>(e).position = {x, 0.0f, 0.0f};
    }
    const auto wall = scene->createEntity("Wall");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(wall);
    mesh.baseColor = glm::vec3(0.0f);
    mesh.roughness = 1.0f;
    auto& t = scene->registry().get<TransformComponent>(wall);
    t.position = {1.5f, 0.0f, 1.2f};
    t.scale = {1.6f, 1.6f, 0.1f};
    scene->updateWorldTransforms();
    updateParticles(*scene, 0.001f);
    ASSERT_EQ(particleCount(*scene), 400u);

    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(160, 80);
    const CameraView view = cam.view(2.0f);
    RenderSettings settings;
    settings.bloom = false;
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;
    EXPECT_EQ(renderer_->stats().scene.particles, 400u);
    const glm::ivec2 left = toPixel(view, glm::vec3(-1.5f, 0.0f, 0.0f), size);
    const glm::ivec2 right = toPixel(view, glm::vec3(1.5f, 0.0f, 0.0f), size);
    const auto lit = pixel(*image, left.x, left.y);
    const auto hidden = pixel(*image, right.x, right.y);
    EXPECT_GT(lit.r, 150) << "additive particles add up";
    EXPECT_GT(lit.r, lit.b + 60) << "in their colour";
    EXPECT_LT(luminance(hidden), 20.0f) << "behind the wall";
}

TEST_F(RendererTest, DebugShapesDrawOnTop) {
    auto scene = sceneWithSun();
    const auto wall = scene->createEntity("Wall");
    scene->registry().emplace<MeshRendererComponent>(wall).baseColor = glm::vec3(0.2f);
    scene->registry().get<TransformComponent>(wall).scale = {6.0f, 6.0f, 0.2f};
    scene->updateWorldTransforms();
    DebugDraw debug;
    debug.lines.push_back({{-2.0f, 0.0f, -1.0f}, {2.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 1.0f, 1.0f}});  // behind the wall
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(128, 64);
    const CameraView view = cam.view(2.0f);
    RenderSettings settings;
    settings.bloom = false;
    settings.debug = &debug;
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value());
    int magenta = 0;
    for (std::uint32_t x = 0; x < size.x; ++x) {
        for (std::uint32_t y = 26; y < 38; ++y) {
            const auto c = pixel(*image, x, y);
            if (c.r > 150 && c.b > 150 && c.g < 90) ++magenta;
        }
    }
    EXPECT_GT(magenta, 40) << "the line shows through the wall, a couple of pixels thick";
}

TEST_F(RendererTest, SpotAndPointLightsCastShadows) {
    // A dark floor, a cube 1 m above it, a light above the cube. With castShadows the floor right
    // under the cube is dark while the floor beside it is lit; without, both are lit.
    for (const LightType type : {LightType::Spot, LightType::Point}) {
        auto scene = std::make_unique<Scene>("Local shadows");
        scene->environment().ambientIntensity = 0.0f;
        scene->environment().fogDensity = 0.0f;
        const auto floor = scene->createEntity("Floor");
        auto& mesh = scene->registry().emplace<MeshRendererComponent>(floor);
        mesh.mesh = MeshPrimitive::Plane;
        mesh.baseColor = glm::vec3(0.8f);
        mesh.roughness = 1.0f;
        scene->registry().get<TransformComponent>(floor).scale = glm::vec3(20.0f, 1.0f, 20.0f);
        const auto cube = scene->createEntity("Cube");
        scene->registry().emplace<MeshRendererComponent>(cube).baseColor = glm::vec3(0.8f);
        scene->registry().get<TransformComponent>(cube).position = {0.0f, 1.5f, 0.0f};
        const auto lamp = scene->createEntity("Lamp");
        auto& light = scene->registry().emplace<LightComponent>(lamp);
        light.type = type;
        light.intensity = 40.0f;
        light.range = 15.0f;
        light.spotAngleDeg = 60.0f;
        light.castShadows = true;
        auto& t = scene->registry().get<TransformComponent>(lamp);
        t.position = {0.0f, 4.0f, 0.0f};
        t.rotationDeg = {-90.0f, 0.0f, 0.0f};
        scene->updateWorldTransforms();

        FlyCameraController cam;
        cam.setPosition({0.0f, 9.0f, 6.0f});
        cam.lookAt({0.0f, 0.0f, 0.0f});
        const glm::uvec2 size(160, 120);
        const CameraView view = cam.view(static_cast<float>(size.x) / static_cast<float>(size.y));
        RenderSettings settings;
        settings.bloom = false;
        settings.ambientOcclusion = false;
        auto shadowed = renderer_->capture(*scene, view, {size.x, size.y}, settings);
        ASSERT_TRUE(shadowed.has_value()) << shadowed.error().message;
        // Under the cube but outside it from this camera: the floor just in front of the cube.
        const glm::ivec2 under = toPixel(view, glm::vec3(0.0f, 0.0f, 0.75f), size);
        const glm::ivec2 beside = toPixel(view, glm::vec3(2.6f, 0.0f, 0.75f), size);
        const float dark = luminance(pixel(*shadowed, under.x, under.y));
        const float lit = luminance(pixel(*shadowed, beside.x, beside.y));
        EXPECT_LT(dark, lit * 0.5f) << (type == LightType::Spot ? "spot" : "point") << " light shadow under the cube";

        scene->registry().get<LightComponent>(lamp).castShadows = false;
        auto unshadowed = renderer_->capture(*scene, view, {size.x, size.y}, settings);
        ASSERT_TRUE(unshadowed.has_value());
        EXPECT_GT(luminance(pixel(*unshadowed, under.x, under.y)), dark * 1.5f) << "no shadow without castShadows";
    }
}

TEST_F(RendererTest, ColorGradingDepthOfFieldAndSunShafts) {
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const auto cube = scene->createEntity("Cube");
    scene->registry().emplace<MeshRendererComponent>(cube).baseColor = {0.9f, 0.1f, 0.1f};
    const auto floor = scene->createEntity("Floor");
    auto& checker = scene->registry().emplace<MeshRendererComponent>(floor);
    checker.mesh = MeshPrimitive::Plane;
    checker.pattern = SurfacePattern::Checker;
    scene->registry().get<TransformComponent>(floor).position.y = -1.0f;
    scene->registry().get<TransformComponent>(floor).scale = glm::vec3(80.0f, 1.0f, 80.0f);
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.5f, 3.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(128, 96);
    const CameraView view = cam.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    RenderSettings settings;
    settings.bloom = false;
    settings.antiAliasing = false;
    auto render = [&] {
        auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
        EXPECT_TRUE(image.has_value());
        return *image;
    };
    const auto plain = render();
    const auto red = pixel(plain, 64, 48);
    ASSERT_GT(red.r, red.g + 40);

    // Saturation 0: grey. Warm temperature: more red than blue on a grey surface.
    scene->environment().saturation = 0.0f;
    const auto grey = pixel(render(), 64, 48);
    EXPECT_LT(std::abs(int(grey.r) - int(grey.g)), 4);
    scene->environment().saturation = 1.0f;

    // Depth of field focused on the cube: the far checkerboard loses contrast, the cube keeps it.
    // High-frequency energy (neighbour differences) of the far floor beside the cube.
    auto detail = [&](const CapturedImage& img) {
        float sum = 0.0f;
        for (int y = 40; y < 46; ++y) {
            for (int x = 4; x < 40; ++x) sum += std::abs(luminance(pixel(img, x + 1, y)) - luminance(pixel(img, x, y)));
        }
        return sum;
    };
    const float farSharp = detail(plain);
    ASSERT_GT(farSharp, 100.0f) << "the checkerboard has detail to blur";
    scene->environment().dofBlur = 12.0f;
    scene->environment().focusDistance = 3.0f;
    scene->environment().focusRange = 1.5f;
    const auto blurred = render();
    EXPECT_LT(detail(blurred), farSharp * 0.6f) << "far floor blurred";
    const auto cubeAfter = pixel(blurred, 64, 52);
    EXPECT_GT(cubeAfter.r, cubeAfter.g + 40) << "the cube in focus stays sharp and red";
    scene->environment().dofBlur = 0.0f;

    // Sun shafts: with the sun in front of the camera the image brightens; behind it nothing.
    const auto sun = scene->createEntity("Low sun");
    scene->registry().emplace<LightComponent>(sun).intensity = 4.0f;
    scene->registry().get<TransformComponent>(sun).rotationDeg = {-10.0f, 0.0f, 0.0f};  // shining towards -Z... from +Z
    scene->updateWorldTransforms();
    auto total = [](const CapturedImage& img) {
        double sum = 0.0;
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) sum += img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2];
        return sum;
    };
    for (auto e : scene->registry().view<LightComponent>()) {
        if (e != sun) scene->registry().remove<LightComponent>(e);
    }
    scene->updateWorldTransforms();
    // Look along the sun's light, back towards it.
    FlyCameraController towards;
    towards.setPosition({0.0f, 0.5f, -3.0f});
    towards.lookAt({0.0f, 1.2f, 3.0f});
    const CameraView sunView = towards.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    scene->environment().sunShafts = 0.0f;
    auto noShafts = renderer_->capture(*scene, sunView, {size.x, size.y}, settings);
    scene->environment().sunShafts = 2.0f;
    auto withShafts = renderer_->capture(*scene, sunView, {size.x, size.y}, settings);
    ASSERT_TRUE(noShafts && withShafts);
    EXPECT_GT(total(*withShafts), total(*noShafts) * 1.02) << "shafts add light";
}

TEST_F(RendererTest, DecalsTintTheSurfacesInsideTheirBox) {
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const auto floor = scene->createEntity("Floor");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(floor);
    mesh.mesh = MeshPrimitive::Plane;
    mesh.baseColor = glm::vec3(0.8f);
    scene->registry().get<TransformComponent>(floor).scale = glm::vec3(20.0f, 1.0f, 20.0f);
    const auto decal = scene->createEntity("Stain");
    auto& d = scene->registry().emplace<DecalComponent>(decal);
    d.color = {0.1f, 0.2f, 1.0f};
    scene->registry().get<TransformComponent>(decal).scale = {2.0f, 1.0f, 2.0f};
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 6.0f, 0.01f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(96, 96);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value());
    const glm::ivec2 inside = toPixel(view, glm::vec3(0.0f), size);
    const glm::ivec2 outside = toPixel(view, glm::vec3(2.5f, 0.0f, 0.0f), size);
    const auto in = pixel(*image, inside.x, inside.y);
    const auto out = pixel(*image, outside.x, outside.y);
    EXPECT_GT(in.b, in.r + 40) << "blue inside the box";
    EXPECT_LT(std::abs(int(out.r) - int(out.b)), 15) << "untouched outside";
    scene->registry().get<DecalComponent>(decal).opacity = 0.0f;
    auto faded = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(faded.has_value());
    const auto clear = pixel(*faded, inside.x, inside.y);
    EXPECT_LT(std::abs(int(clear.r) - int(clear.b)), 15) << "opacity 0: no decal";
}

// ---- HUD ------------------------------------------------------------------------------------

#include "renderer/HudRenderer.hpp"

TEST(HudFont, MeasuresTextFromTheEmbeddedFont) {
    HudFont font;
    EXPECT_GT(font.measure("Hello", 24.0f), 40.0f);
    EXPECT_NEAR(font.measure("Hello", 48.0f), 2.0f * font.measure("Hello", 24.0f), 0.01f) << "scales with size";
    EXPECT_GT(font.measure("WWW", 24.0f), font.measure("iii", 24.0f));
    EXPECT_GT(font.measure("Zażółć gęślą jaźń", 24.0f), 0.0f) << "Polish letters are in the font";
    EXPECT_NEAR(font.measure("ab\nabcd", 24.0f), font.measure("abcd", 24.0f), 0.01f) << "widest line";
    std::vector<HudVertex> vertices;
    std::vector<HudFont::Batch> batches;
    HudCanvas canvas;
    canvas.addRect({0, 0}, {10, 10}, {1, 1, 1, 1});
    canvas.addText({0, 0}, 20.0f, {1, 1, 1, 1}, "A b");
    font.layout(canvas, vertices, batches);
    EXPECT_EQ(vertices.size(), 6u * 3u) << "one quad for the rectangle, one per visible glyph (A, b)";
    EXPECT_EQ(batches.size(), 1u) << "rectangles and glyphs share the atlas";
}

TEST(HudFont, DistanceFieldsForManyScriptsMadeOnFirstUse) {
    HudFont font;
    font.takeDirty();
    const std::size_t before = font.glyphCount();
    EXPECT_GT(font.measure("Привет мир", 24.0f), 50.0f) << "Cyrillic";
    EXPECT_GT(font.measure("Καλημέρα", 24.0f), 50.0f) << "Greek";
    EXPECT_GT(font.glyphCount(), before) << "glyphs are made when first needed";
    EXPECT_TRUE(font.takeDirty()) << "and the atlas must be uploaded again";
    EXPECT_FALSE(font.takeDirty());
    // A glyph no font has: a question mark (same width), not nothing.
    EXPECT_NEAR(font.measure("\u4E2D", 24.0f), font.measure("?", 24.0f), 0.01f);
    // Sharp at any size: the quads grow with the text, from one field per glyph.
    std::vector<HudVertex> small, large;
    std::vector<HudFont::Batch> b1, b2;
    HudCanvas a, b;
    a.addText({0, 0}, 12.0f, {1, 1, 1, 1}, "W");
    b.addText({0, 0}, 96.0f, {1, 1, 1, 1}, "W");
    font.layout(a, small, b1);
    font.layout(b, large, b2);
    ASSERT_EQ(small.size(), 6u);
    ASSERT_EQ(large.size(), 6u);
    EXPECT_NEAR((large[2].position.x - large[0].position.x) / (small[2].position.x - small[0].position.x), 8.0f, 0.01f);
    EXPECT_EQ(large[0].uv, small[0].uv);
    // A project font: bytes that are not a font fall back to the built-in one.
    EXPECT_EQ(font.addFont("Content/Fonts/Broken.ttf", std::vector<std::byte>(16)), 0);
    EXPECT_EQ(font.findFont("Content/Fonts/Broken.ttf"), 0);
    EXPECT_EQ(font.findFont("Content/Fonts/Unknown.ttf"), -1);
}

TEST(HudFont, PicturesSplitBatchesByTexture) {
    HudFont font;
    HudCanvas canvas;
    canvas.addImage({0, 0}, {10, 10}, "a.png");
    canvas.addImage({10, 0}, {10, 10}, "a.png");
    canvas.addRect({0, 0}, {5, 5}, {1, 1, 1, 1});
    canvas.addImage({20, 0}, {10, 10}, "b.png");
    canvas.addText({0, 20}, 20.0f, {1, 1, 1, 1}, "x");
    canvas.addImage({30, 0}, {10, 10}, "missing.png");
    std::vector<HudVertex> vertices;
    std::vector<HudFont::Batch> batches;
    font.layout(canvas, vertices, batches, [](const std::string& key) -> std::uint32_t {
        return key == "a.png" ? 7u : key == "b.png" ? 9u : UINT32_MAX;
    });
    ASSERT_EQ(batches.size(), 3u) << "a.png (both, and the rectangle after them), b.png, the text";
    EXPECT_TRUE(batches[0].picture);
    EXPECT_EQ(batches[0].texture, 7u);
    EXPECT_EQ(batches[0].count, 18u);
    EXPECT_EQ(batches[1].texture, 9u);
    EXPECT_FALSE(batches[2].picture);
    EXPECT_EQ(vertices.size(), 30u) << "the missing picture is skipped";
}

TEST_F(RendererTest, HudAlignsTextAndKeepsDrawingOrder) {
    // White text centred in the right half; a black rectangle added after the left text covers
    // it (later items on top), while the right text, added after its own rectangle, shows.
    auto scene = sceneWithSun();
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 1.0f, 5.0f});
    HudCanvas hud;
    hud.screenSize = {200.0f, 100.0f};
    hud.addText({0.0f, 30.0f}, 40.0f, {1.0f, 1.0f, 1.0f, 1.0f}, "MM");
    hud.addRect({0.0f, 0.0f}, {100.0f, 100.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    hud.addRect({100.0f, 0.0f}, {100.0f, 100.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    hud.addText({100.0f, 30.0f}, 40.0f, {1.0f, 1.0f, 1.0f, 1.0f}, "MM", HudCanvas::Align::Center, 100.0f);
    RenderSettings settings;
    settings.bloom = false;
    settings.hud = &hud;
    auto image = renderer_->capture(*scene, cam.view(2.0f), {200, 100}, settings);
    ASSERT_TRUE(image.has_value());
    int left = 0, rightNear = 0, rightFar = 0;
    float sumX = 0.0f;
    for (std::uint32_t y = 0; y < 100; ++y) {
        for (std::uint32_t x = 0; x < 200; ++x) {
            if (luminance(pixel(*image, x, y)) < 128.0f) continue;
            if (x < 100) {
                ++left;
            } else {
                ++((x >= 115 && x < 185) ? rightNear : rightFar);
                sumX += static_cast<float>(x);
            }
        }
    }
    EXPECT_EQ(left, 0) << "the rectangle added later hides the left text";
    ASSERT_GT(rightNear, 40);
    EXPECT_EQ(rightFar, 0) << "centred text stays clear of the box edges";
    EXPECT_NEAR(sumX / static_cast<float>(rightNear), 150.0f, 6.0f) << "centred on the box";
}

TEST_F(RendererTest, HudDrawsRectanglesAndTextOnTop) {
    auto scene = sceneWithSun();
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 1.0f, 5.0f});
    HudCanvas hud;
    hud.screenSize = {200.0f, 100.0f};
    hud.addRect({10.0f, 10.0f}, {40.0f, 20.0f}, {1.0f, 0.0f, 0.0f, 1.0f});
    hud.addText({80.0f, 40.0f}, 40.0f, {1.0f, 1.0f, 1.0f, 1.0f}, "HUD");
    RenderSettings off;
    off.bloom = false;
    RenderSettings on = off;
    on.hud = &hud;
    auto plain = renderer_->capture(*scene, cam.view(2.0f), {200, 100}, off);
    auto drawn = renderer_->capture(*scene, cam.view(2.0f), {200, 100}, on);
    ASSERT_TRUE(plain.has_value() && drawn.has_value());
    const auto rect = pixel(*drawn, 30, 20);
    EXPECT_GT(rect.r, 200);
    EXPECT_LT(rect.g, 30);
    EXPECT_LT(rect.b, 30);
    int textPixels = 0, changedElsewhere = 0;
    for (std::uint32_t y = 0; y < 100; ++y) {
        for (std::uint32_t x = 0; x < 200; ++x) {
            const bool changed = glm::ivec3(pixel(*drawn, x, y)) != glm::ivec3(pixel(*plain, x, y));
            const bool inRect = x >= 10 && x < 50 && y >= 10 && y < 30;
            const bool inText = x >= 78 && x < 160 && y >= 38 && y < 84;
            if (inText && changed && luminance(pixel(*drawn, x, y)) > 200.0f) ++textPixels;
            if (!inRect && !inText && changed) ++changedElsewhere;
        }
    }
    EXPECT_GT(textPixels, 60) << "white glyph pixels where the text is";
    EXPECT_EQ(changedElsewhere, 0) << "nothing else changes";
}

TEST(Sprites, LayoutFacesTheViewerSortsFarFirstAndPicksTheNearest) {
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(2.0f);
    SpriteAtlas atlas;
    atlas.width = atlas.height = 1;
    atlas.rgba = {255, 255, 255, 255};
    SpriteBatch batch;
    batch.atlas = &atlas;
    batch.pixelRatio = 2.0f;
    batch.sprites.push_back({{0.0f, 0.0f, 0.0f}, 10.0f, {0, 0, 1, 1}, glm::vec4(1.0f), 1});   // centre
    batch.sprites.push_back({{0.0f, 0.0f, -5.0f}, 10.0f, {0, 0, 1, 1}, glm::vec4(1.0f), 2});  // behind it, farther
    batch.sprites.push_back({{0.0f, 0.0f, 9.0f}, 10.0f, {0, 0, 1, 1}, glm::vec4(1.0f), 3});   // behind the camera
    const auto quads = layoutSprites(batch, view, {400.0f, 200.0f});
    ASSERT_EQ(quads.size(), 2u);
    EXPECT_EQ(batch.sprites[quads[0].index].id, 2u) << "farther first";
    EXPECT_EQ(batch.sprites[quads[1].index].id, 1u);
    EXPECT_NEAR(quads[1].min.x, 190.0f, 1.0f) << "20 target pixels wide (10 x pixel ratio 2), centred";
    EXPECT_NEAR(quads[1].max.x - quads[1].min.x, 20.0f, 1e-3f);
    EXPECT_NEAR(quads[1].min.y, 90.0f, 1.0f);
    const Sprite* hit = spriteAt(batch, view, {400.0f, 200.0f}, {200.0f, 100.0f});
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->id, 1u) << "the nearer of the two overlapping sprites";
    EXPECT_EQ(spriteAt(batch, view, {400.0f, 200.0f}, {20.0f, 20.0f}), nullptr);
}

TEST_F(RendererTest, SpritesDrawTheirPictureTintedOnTop) {
    auto scene = sceneWithSun();
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    // Left half white, right half transparent.
    SpriteAtlas atlas;
    atlas.width = 2;
    atlas.height = 1;
    atlas.rgba = {255, 255, 255, 255, 255, 255, 255, 0};
    atlas.version = 1;
    SpriteBatch batch;
    batch.atlas = &atlas;
    batch.sprites.push_back({{0.0f, 0.0f, 0.0f}, 40.0f, {0.0f, 0.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, 1});
    batch.sprites.push_back({{3.0f, 0.0f, 0.0f}, 40.0f, {0.75f, 0.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, 2});
    RenderSettings off;
    off.bloom = false;
    off.antiAliasing = false;
    RenderSettings on = off;
    on.sprites = &batch;
    auto plain = renderer_->capture(*scene, cam.view(2.0f), {200, 100}, off);
    auto drawn = renderer_->capture(*scene, cam.view(2.0f), {200, 100}, on);
    ASSERT_TRUE(plain.has_value() && drawn.has_value());
    const auto centre = pixel(*drawn, 100, 50);
    EXPECT_LT(centre.r, 30);
    EXPECT_GT(centre.g, 220) << "white picture tinted green";
    EXPECT_LT(centre.b, 30);
    // The second sprite samples only the transparent texel: nothing changes there.
    const glm::vec4 clip = cam.view(2.0f).projection * cam.view(2.0f).view * glm::vec4(3.0f, 0.0f, 0.0f, 1.0f);
    const auto x = static_cast<std::uint32_t>((clip.x / clip.w * 0.5f + 0.5f) * 200.0f);
    EXPECT_EQ(glm::ivec3(pixel(*drawn, x, 50)), glm::ivec3(pixel(*plain, x, 50)));
    EXPECT_EQ(glm::ivec3(pixel(*drawn, 5, 5)), glm::ivec3(pixel(*plain, 5, 5))) << "nothing else changes";

    // Off the centre: the picture sits exactly where its point projects (above stays above).
    for (const glm::vec3 at : {glm::vec3(-2.0f, 1.0f, 0.0f), glm::vec3(1.5f, -1.0f, 0.0f)}) {
        batch.sprites = {{at, 10.0f, {0.0f, 0.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, 3}};
        auto moved = renderer_->capture(*scene, cam.view(2.0f), {200, 100}, on);
        ASSERT_TRUE(moved.has_value());
        const glm::ivec2 p = toPixel(cam.view(2.0f), at, {200, 100});
        const auto green = pixel(*moved, static_cast<std::uint32_t>(p.x), static_cast<std::uint32_t>(p.y));
        EXPECT_GT(static_cast<int>(green.g), static_cast<int>(green.r) + 150) << "the green picture at " << at.x << ", " << at.y;
        const auto quad = layoutSprites(batch, cam.view(2.0f), {200.0f, 100.0f});
        ASSERT_EQ(quad.size(), 1u);
        EXPECT_NEAR((quad[0].min.y + quad[0].max.y) * 0.5f, static_cast<float>(p.y), 1.5f) << "layout and picking agree";
    }
}

// ---- Terrain, foliage, water ------------------------------------------------------------------

TEST_F(RendererTest, TerrainIsGrassOnFlatGroundAndRockOnSlopes) {
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const auto ground = scene->createEntity("Ground");
    auto& t = scene->registry().emplace<TerrainComponent>(ground);
    t.size = 40.0f;
    t.height = 0.0f;  // flat
    t.resolution = 33;
    t.grassColor = {0.0f, 0.8f, 0.0f};
    t.rockColor = {0.8f, 0.0f, 0.0f};
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 10.0f, 0.01f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(64, 64);
    RenderSettings settings;
    settings.bloom = false;
    auto flat = renderer_->capture(*scene, cam.view(1.0f), {size.x, size.y}, settings);
    ASSERT_TRUE(flat.has_value());
    const auto f = pixel(*flat, 32, 32);
    EXPECT_GT(f.g, f.r + 30) << "grass";
    t.height = 60.0f;  // steep hills everywhere
    t.hillSize = 6.0f;
    auto steep = renderer_->capture(*scene, cam.view(1.0f), {size.x, size.y}, settings);
    ASSERT_TRUE(steep.has_value());
    int red = 0, green = 0;
    for (std::uint32_t y = 8; y < 56; ++y) {
        for (std::uint32_t x = 8; x < 56; ++x) {
            const auto p = pixel(*steep, x, y);
            red += p.r > p.g ? 1 : 0;
            green += p.g > p.r ? 1 : 0;
        }
    }
    EXPECT_GT(red, green) << "mostly rock on steep ground";
}

TEST_F(RendererTest, FoliageCopiesAreDrawnInstanced) {
    auto scene = sceneWithSun();
    const auto patch = scene->createEntity("Rocks");
    auto& f = scene->registry().emplace<FoliageComponent>(patch);
    f.count = 400;
    f.radius = 6.0f;
    f.minScale = f.maxScale = 0.2f;
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 25.0f, 0.01f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    RenderSettings settings;
    settings.shadows = false;
    auto image = renderer_->capture(*scene, cam.view(1.0f), {64, 64}, settings);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(renderer_->stats().scene.visibleObjects, 400u);
    EXPECT_LE(renderer_->stats().scene.drawCalls, 10u) << "400 copies in one instanced draw per pass";
}

TEST_F(RendererTest, WaterIsDeeperColouredAndHidesBehindGeometry) {
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    // A white floor sloping from 0.2 m to 8 m under the water.
    const auto floor = scene->createEntity("Floor");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(floor);
    mesh.mesh = MeshPrimitive::Plane;
    mesh.baseColor = glm::vec3(0.9f);
    auto& ft = scene->registry().get<TransformComponent>(floor);
    ft.scale = {30.0f, 1.0f, 30.0f};
    ft.position.y = -4.0f;
    ft.rotationDeg.z = -15.0f;  // deeper towards +X
    const auto pool = scene->createEntity("Pool");
    auto& w = scene->registry().emplace<WaterComponent>(pool);
    w.width = w.length = 30.0f;
    w.waveHeight = 0.0f;
    w.shallowColor = {0.0f, 0.6f, 0.6f};
    w.deepColor = {0.0f, 0.0f, 0.3f};
    w.clarity = 2.0f;
    // A red box sticking out of the water.
    const auto box = scene->createEntity("Box");
    scene->registry().emplace<MeshRendererComponent>(box).baseColor = {0.9f, 0.0f, 0.0f};
    scene->registry().get<TransformComponent>(box).position = {-3.0f, 0.5f, 0.0f};
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 14.0f, 0.01f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(96, 96);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    settings.shadows = false;
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value());
    const glm::ivec2 shallow = toPixel(view, glm::vec3(-8.0f, 0.0f, 3.0f), size);
    const glm::ivec2 deep = toPixel(view, glm::vec3(8.0f, 0.0f, 3.0f), size);
    const glm::ivec2 top = toPixel(view, glm::vec3(-3.0f, 1.0f, 0.0f), size);
    const auto s = pixel(*image, shallow.x, shallow.y);
    const auto d = pixel(*image, deep.x, deep.y);
    const auto b = pixel(*image, top.x, top.y);
    EXPECT_GT(int(s.g), int(d.g) + 20) << "shallow water is lighter and greener than deep water";
    EXPECT_GT(d.b, d.r) << "deep water is blue";
    EXPECT_GT(b.r, b.b + 60) << "the box above the water is not covered by it";
}

TEST(MipChain, CutOutTexturesKeepTheirColourAndCoverage) {
    // A 64 x 64 leaf: green disc (alpha 255) on transparent white.
    constexpr std::uint32_t size = 64;
    std::vector<std::uint8_t> rgba(size * size * 4);
    std::size_t covered = 0;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) - 31.5f, dy = static_cast<float>(y) - 31.5f;
            const bool leaf = dx * dx + dy * dy < 14.0f * 14.0f;
            std::uint8_t* p = &rgba[(y * size + x) * 4];
            p[0] = leaf ? 30 : 255;
            p[1] = leaf ? 140 : 255;
            p[2] = leaf ? 40 : 255;
            p[3] = leaf ? 255 : 0;
            covered += leaf ? 1 : 0;
        }
    }
    const auto mips = buildMipChain(rgba, size, size, true);
    ASSERT_EQ(mips.size(), 7u);
    const float target = static_cast<float>(covered) / static_cast<float>(size * size);
    for (std::size_t l = 1; l + 3 < mips.size(); ++l) {  // down to 8 x 8 (coarser levels are too few texels)
        const auto& level = mips[l];
        std::size_t above = 0;
        int whitest = 0;
        for (std::size_t i = 0; i < level.size(); i += 4) {
            above += level[i + 3] >= 128 ? 1 : 0;
            if (level[i + 3] > 0) whitest = std::max(whitest, static_cast<int>(level[i]));  // red stays low: no white mixed in
        }
        EXPECT_NEAR(static_cast<float>(above) / static_cast<float>(level.size() / 4), target, 0.06f) << "level " << l;
        EXPECT_LT(whitest, 60) << "level " << l << ": the transparent white does not bleed into the leaf";
    }
    // Under the transparent texels sits the leaf's colour, not white.
    EXPECT_LT(mips[0][0], 60);
    EXPECT_EQ(mips[0][3], 0) << "full-size alpha unchanged";
}

TEST_F(RendererTest, ComputePassesWriteStorageImagesThatLaterPassesUse) {
    auto scene = sceneWithSun();
    scene->updateWorldTransforms();
    FlyCameraController cam;
    constexpr std::uint32_t w = 64, h = 32;
    nvrhi::ComputePipelineHandle pattern = context_->createComputePipeline("pattern.comp");
    ASSERT_TRUE(pattern);
    nvrhi::IDescriptorTable* bindless = context_->bindless().table();
    auto image = renderer_->capture(*scene, cam.view(2.0f), {w, h}, {}, [&](RenderGraph& graph, RGImage out) {
        const RGImage target = graph.createImage("Pattern", {nvrhi::Format::RGBA8_UNORM, w, h, true});
        graph.addComputePass("Pattern", [&](PassBuilder& b) { b.writeStorage(target); },
                             [&, target](PassContext& ctx) {
                                 nvrhi::ComputeState state;
                                 state.pipeline = pattern;
                                 state.bindings = {ctx.frameBindings(), bindless, ctx.storageBindings()};
                                 ctx.cmd->setComputeState(state);
                                 PushConstants pc;
                                 pc.index0 = w;
                                 pc.index1 = h;
                                 pc.param0 = 0.25f;
                                 ctx.cmd->setPushConstants(&pc, sizeof(pc));
                                 ctx.cmd->dispatch((w + 7) / 8, (h + 7) / 8, 1);
                             });
        graph.addPass(
            "CopyPattern",
            [&](PassBuilder& b) {
                b.copyFrom(target);
                b.copyTo(out);
            },
            [target, out](PassContext& ctx) { ctx.cmd->copyTexture(ctx.texture(out), {}, ctx.texture(target), {}); });
    });
    ASSERT_TRUE(image.has_value()) << image.error().message;
    for (const glm::uvec2 p : {glm::uvec2(0, 0), glm::uvec2(32, 16), glm::uvec2(63, 31)}) {
        const auto c = pixel(*image, p.x, p.y);
        EXPECT_NEAR(c.r, 255.0f * static_cast<float>(p.x) / w, 1.5f) << p.x << "," << p.y;
        EXPECT_NEAR(c.g, 255.0f * static_cast<float>(p.y) / h, 1.5f);
        EXPECT_NEAR(c.b, 64.0f, 1.5f);
    }
}

namespace {
/// A dark cube turned 30 degrees against the bright sky: slanted, high-contrast edges.
std::unique_ptr<Scene> edgeScene(entt::entity* cubeOut = nullptr) {
    auto scene = std::make_unique<Scene>("Edges");
    scene->environment().fogDensity = 0.0f;
    const auto cube = scene->createEntity("Cube");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(cube);
    mesh.baseColor = glm::vec3(0.02f);
    mesh.roughness = 1.0f;
    scene->registry().get<TransformComponent>(cube).rotationDeg = {0.0f, 0.0f, 30.0f};
    scene->updateWorldTransforms();
    if (cubeOut != nullptr) *cubeOut = cube;
    return scene;
}

/// Anti-aliased edge pixels: between the cube's dark and the sky, right next to the cube.
int partialPixels(const CapturedImage& image) {
    const auto green = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
        return static_cast<int>(image.rgba[(static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4 + 1]);
    };
    int count = 0;
    for (int y = 0; y < static_cast<int>(image.height); ++y) {
        for (int x = 0; x < static_cast<int>(image.width); ++x) {
            const int g = green(x, y);
            if (g < 35 || g > 110) continue;
            bool nearCube = false;
            for (int dy = -1; dy <= 1 && !nearCube; ++dy) {
                for (int dx = -1; dx <= 1 && !nearCube; ++dx) nearCube = green(x + dx, y + dy) < 25;
            }
            count += nearCube ? 1 : 0;
        }
    }
    return count;
}
} // namespace

TEST_F(RendererTest, TemporalAntiAliasingSmoothsEdgesAndSettles) {
    auto scene = edgeScene();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 3.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    RenderSettings plain;
    plain.antiAliasing = false;
    plain.bloom = false;
    plain.shadows = false;
    auto aliased = renderer_->capture(*scene, view, {96, 96}, plain);
    ASSERT_TRUE(aliased.has_value());

    RenderSettings taa = plain;
    taa.temporalAntiAliasing = true;
    std::optional<CapturedImage> frame;
    for (int i = 0; i < 24; ++i) {
        auto f = renderer_->capture(*scene, view, {96, 96}, taa);
        ASSERT_TRUE(f.has_value());
        frame = std::move(*f);
    }
    EXPECT_TRUE(renderer_->stats().scene.temporalAntiAliasing);
    EXPECT_GT(partialPixels(*frame), partialPixels(*aliased) * 2) << "edges are blended over the jittered frames";

    // Settled: more frames change the image only a little.
    auto next = renderer_->capture(*scene, view, {96, 96}, taa);
    ASSERT_TRUE(next.has_value());
    int changed = 0;
    for (std::size_t i = 0; i < next->rgba.size(); i += 4) changed += std::abs(next->rgba[i + 1] - frame->rgba[i + 1]) > 24 ? 1 : 0;
    EXPECT_LT(changed, 20) << "no crawling once converged";
}

TEST_F(RendererTest, TemporalAntiAliasingLeavesNoTrailBehindMovingObjects) {
    entt::entity cube = entt::null;
    auto scene = edgeScene(&cube);
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 3.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    RenderSettings taa;
    taa.antiAliasing = false;
    taa.bloom = false;
    taa.shadows = false;
    taa.temporalAntiAliasing = true;
    for (int i = 0; i < 12; ++i) ASSERT_TRUE(renderer_->capture(*scene, view, {96, 96}, taa).has_value());
    // The cube jumps aside; a few frames later its old place shows the sky again.
    scene->registry().get<TransformComponent>(cube).position.x = 1.6f;
    scene->updateWorldTransforms();
    std::optional<CapturedImage> frame;
    for (int i = 0; i < 4; ++i) {
        auto f = renderer_->capture(*scene, view, {96, 96}, taa);
        ASSERT_TRUE(f.has_value());
        frame = std::move(*f);
    }
    const auto oldPlace = pixel(*frame, 48, 48);
    const auto sky = pixel(*frame, 10, 48);
    EXPECT_GT(oldPlace.g, sky.g - 30) << "no ghost of the cube where it was";
    // ...and the cube is drawn where it went.
    const glm::ivec2 now = toPixel(view, {1.6f, 0.0f, 0.0f}, {96, 96});
    EXPECT_LT(pixel(*frame, static_cast<std::uint32_t>(std::clamp(now.x, 0, 95)), 48).g, 60);
}

TEST_F(RendererTest, BlockCompressedTexturesUploadRowsOfBlocks) {
    // A 8 x 8 BC1 texture: 2 x 2 blocks of 8 bytes, every block solid red (colour0 = colour1 = red).
    std::vector<std::byte> level(4 * 8);
    for (std::size_t b = 0; b < 4; ++b) {
        level[b * 8 + 0] = std::byte{0x00};
        level[b * 8 + 1] = std::byte{0xF8};  // RGB565 red, little-endian 0xF800
        level[b * 8 + 2] = std::byte{0x00};
        level[b * 8 + 3] = std::byte{0xF8};
    }
    const std::span<const std::byte> levels[] = {level};
    nvrhi::TextureHandle texture;
    EXPECT_NO_THROW(texture = context_->createTexture(levels, 8, 8, nvrhi::Format::BC1_UNORM, "BC1Test"));
    EXPECT_TRUE(texture);
}

TEST_F(RendererTest, TransparentSurfacesBlendBackToFrontAndHideBehindGeometry) {
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    auto plate = [&](const char* name, glm::vec3 position, glm::vec3 scale, glm::vec3 color, float opacity) {
        const auto e = scene->createEntity(name);
        auto& m = scene->registry().emplace<MeshRendererComponent>(e);
        m.mesh = MeshPrimitive::Plane;
        m.baseColor = glm::vec3(0.0f);
        m.roughness = 1.0f;
        m.emissive = color;  // unlit colours: the blend is easy to read
        m.emissiveStrength = 1.0f;
        m.opacity = opacity;
        auto& t = scene->registry().get<TransformComponent>(e);
        t.position = position;
        t.scale = scale;
        return e;
    };
    plate("Floor", {0.0f, 0.0f, 0.0f}, {20.0f, 1.0f, 20.0f}, {1.0f, 0.0f, 0.0f}, 1.0f);
    // Half see-through blue over the left of the floor; green over blue on the right (created
    // first, so only sorting puts the nearer green last).
    plate("Green", {3.0f, 2.0f, 0.0f}, {4.0f, 1.0f, 4.0f}, {0.0f, 1.0f, 0.0f}, 0.75f);
    plate("Blue", {-1.0f, 1.0f, 0.0f}, {10.0f, 1.0f, 4.0f}, {0.0f, 0.0f, 1.0f}, 0.5f);
    // An opaque white box on the floor, partly under the blue plate's far side but above it.
    const auto box = scene->createEntity("Box");
    auto& bm = scene->registry().emplace<MeshRendererComponent>(box);
    bm.baseColor = glm::vec3(0.0f);
    bm.emissive = glm::vec3(1.0f);
    bm.emissiveStrength = 1.0f;
    auto& bt = scene->registry().get<TransformComponent>(box);
    bt.position = {-4.0f, 1.5f, 0.0f};
    bt.scale = glm::vec3(1.0f, 3.0f, 1.0f);  // pokes through the blue plate
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 14.0f, 0.01f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(96, 96);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    settings.shadows = false;
    settings.ambientOcclusion = false;
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(renderer_->stats().scene.transparentObjects, 2u);
    auto at = [&](glm::vec3 world) {
        const glm::ivec2 p = toPixel(view, world, size);
        return pixel(*image, p.x, p.y);
    };
    const auto floor = at({0.0f, 0.0f, 6.0f});
    const auto blue = at({-2.0f, 1.0f, 0.0f});
    const auto both = at({3.0f, 2.0f, 0.0f});
    const auto top = at({-4.0f, 3.0f, 0.0f});
    EXPECT_GT(floor.r, 200);
    EXPECT_LT(floor.b, 30);
    EXPECT_GT(blue.r, 60) << "the floor shows through the blue plate";
    EXPECT_GT(blue.b, 60) << "the blue plate tints it";
    EXPECT_GT(both.g, both.b + 40) << "the nearer green plate is blended last";
    EXPECT_GT(both.b, 20) << "the blue plate shows through the green one";
    EXPECT_GT(top.r, 200);
    EXPECT_GT(top.g, 200) << "the box above the plate is not tinted by it";
}

TEST_F(RendererTest, ScreenSpaceReflectionsShowTheSceneInGlossyFloors) {
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const auto floor = scene->createEntity("Floor");
    auto& fm = scene->registry().emplace<MeshRendererComponent>(floor);
    fm.mesh = MeshPrimitive::Plane;
    fm.baseColor = glm::vec3(0.95f);
    fm.metallic = 1.0f;  // a mirror: all of its colour is reflection
    fm.roughness = 0.05f;
    scene->registry().get<TransformComponent>(floor).scale = {40.0f, 1.0f, 40.0f};
    const auto box = scene->createEntity("Box");
    auto& bm = scene->registry().emplace<MeshRendererComponent>(box);
    bm.baseColor = glm::vec3(0.0f);
    bm.emissive = {1.0f, 0.0f, 0.0f};
    bm.emissiveStrength = 2.0f;
    scene->registry().get<TransformComponent>(box).position = {0.0f, 0.5f, 0.0f};
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 2.0f, 8.0f});
    cam.lookAt({0.0f, 0.5f, 0.0f});
    const glm::uvec2 size(128, 96);
    const CameraView view = cam.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    RenderSettings settings;
    settings.bloom = false;
    settings.shadows = false;
    settings.ambientOcclusion = false;
    auto plain = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(plain.has_value());
    EXPECT_FALSE(renderer_->stats().scene.screenSpaceReflections);
    settings.screenSpaceReflections = true;
    auto reflective = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(reflective.has_value());
    EXPECT_TRUE(renderer_->stats().scene.screenSpaceReflections);
    // The box's mirror image: its centre reflected below the floor.
    const glm::ivec2 mirrored = toPixel(view, {0.0f, -0.5f, 0.0f}, size);
    const glm::ivec2 open = toPixel(view, {3.0f, -0.5f, 0.0f}, size);  // beside it: sky in the floor
    const auto without = pixel(*plain, mirrored.x, mirrored.y);
    const auto with = pixel(*reflective, mirrored.x, mirrored.y);
    EXPECT_LT(int(without.r), int(without.b) + 20) << "without SSR the floor reflects only the sky";
    EXPECT_GT(int(with.r), int(with.b) + 60) << "with SSR the red box shows in the floor";
    const auto beside = pixel(*reflective, open.x, open.y);
    const auto besidePlain = pixel(*plain, open.x, open.y);
    EXPECT_NEAR(int(beside.r), int(besidePlain.r), 12) << "where the ray misses, the environment stays";
    EXPECT_NEAR(int(beside.b), int(besidePlain.b), 12);
}

#include "renderer/ReflectionProbes.hpp"

TEST_F(RendererTest, BakedReflectionProbesReflectTheirSurroundings) {
    test::TempDir project;
    AssetLibrary assets(project.path());
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const auto floor = scene->createEntity("Floor");
    auto& fm = scene->registry().emplace<MeshRendererComponent>(floor);
    fm.mesh = MeshPrimitive::Plane;
    fm.baseColor = glm::vec3(0.95f);
    fm.metallic = 1.0f;
    fm.roughness = 0.05f;
    scene->registry().get<TransformComponent>(floor).scale = {12.0f, 1.0f, 12.0f};
    // A red wall far behind the floor: what the floor's reflection looks at.
    const auto wall = scene->createEntity("Wall");
    auto& wm = scene->registry().emplace<MeshRendererComponent>(wall);
    wm.baseColor = glm::vec3(0.0f);
    wm.emissive = {1.0f, 0.0f, 0.0f};
    wm.emissiveStrength = 2.0f;
    auto& wt = scene->registry().get<TransformComponent>(wall);
    wt.position = {0.0f, 10.0f, -20.0f};
    wt.scale = {60.0f, 20.0f, 1.0f};
    const auto probe = scene->createEntity("Probe");
    auto& p = scene->registry().emplace<ReflectionProbeComponent>(probe);
    p.size = {12.0f, 6.0f, 12.0f};
    p.boxProjection = false;  // open surroundings
    p.resolution = 64;
    scene->registry().get<TransformComponent>(probe).position = {0.0f, 1.0f, 0.0f};
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 3.0f, 6.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(96, 96);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    settings.shadows = false;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    auto unbaked = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(unbaked.has_value());
    EXPECT_EQ(renderer_->stats().scene.reflectionProbes, 0u);

    auto baked = bakeReflectionProbes(*renderer_, *scene, assets, settings);
    ASSERT_TRUE(baked.has_value()) << baked.error().message;
    EXPECT_EQ(*baked, 1);
    const std::string first = scene->registry().get<ReflectionProbeComponent>(probe).baked;
    ASSERT_TRUE(first.starts_with("Saved/Probes/")) << first;
    EXPECT_TRUE(std::filesystem::exists(project.path() / first));

    auto reflecting = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(reflecting.has_value());
    EXPECT_EQ(renderer_->stats().scene.reflectionProbes, 1u);
    const glm::ivec2 onFloor = toPixel(view, {0.0f, 0.0f, 1.0f}, size);
    const auto before = pixel(*unbaked, onFloor.x, onFloor.y);
    const auto after = pixel(*reflecting, onFloor.x, onFloor.y);
    EXPECT_LT(int(before.r), int(before.b) + 20) << "unbaked, the floor reflects the sky";
    EXPECT_GT(int(after.r), int(after.b) + 60) << "baked, it reflects the red wall";

    // Baking again replaces the file.
    auto again = bakeReflectionProbes(*renderer_, *scene, assets, settings);
    ASSERT_TRUE(again.has_value());
    const std::string second = scene->registry().get<ReflectionProbeComponent>(probe).baked;
    EXPECT_NE(second, first);
    EXPECT_TRUE(std::filesystem::exists(project.path() / second));
    EXPECT_FALSE(std::filesystem::exists(project.path() / first));
}

#include "renderer/LightProbes.hpp"

TEST_F(RendererTest, LightProbeVolumesBleedColourOntoNearbySurfaces) {
    test::TempDir project;
    AssetLibrary assets(project.path());
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const auto floor = scene->createEntity("Floor");
    auto& fm = scene->registry().emplace<MeshRendererComponent>(floor);
    fm.mesh = MeshPrimitive::Plane;
    fm.baseColor = glm::vec3(0.9f);
    fm.roughness = 1.0f;
    scene->registry().get<TransformComponent>(floor).scale = {12.0f, 1.0f, 12.0f};
    // A glowing red wall standing on the floor.
    const auto wall = scene->createEntity("Wall");
    auto& wm = scene->registry().emplace<MeshRendererComponent>(wall);
    wm.baseColor = glm::vec3(0.0f);
    wm.emissive = {1.0f, 0.0f, 0.0f};
    wm.emissiveStrength = 4.0f;
    auto& wt = scene->registry().get<TransformComponent>(wall);
    wt.position = {0.0f, 1.5f, -1.5f};
    wt.scale = {6.0f, 3.0f, 0.2f};
    const auto volumeEntity = scene->createEntity("Volume");
    auto& volume = scene->registry().emplace<LightProbeVolumeComponent>(volumeEntity);
    volume.size = {8.0f, 3.0f, 8.0f};
    volume.spacing = 1.5f;
    volume.bounces = 2;
    scene->registry().get<TransformComponent>(volumeEntity).position = {0.0f, 1.5f, 0.0f};
    scene->updateWorldTransforms();

    FlyCameraController cam;
    cam.setPosition({0.0f, 6.0f, 6.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const glm::uvec2 size(96, 96);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    settings.ambientOcclusion = false;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    auto before = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(renderer_->stats().scene.lightProbeVolumes, 0u);

    int reported = 0;
    auto baked = bakeLightProbeVolumes(*renderer_, *scene, assets, settings, false, [&](float) { return ++reported > 0; });
    ASSERT_TRUE(baked.has_value()) << baked.error().message;
    EXPECT_EQ(*baked, 1);
    const glm::uvec3 counts = probeVolumeCounts(volume.size, volume.spacing);
    EXPECT_EQ(reported, static_cast<int>(counts.x * counts.y * counts.z * 2)) << "progress per probe and bounce";
    ASSERT_TRUE(volume.baked.starts_with("Saved/Probes/"));
    EXPECT_TRUE(std::filesystem::exists(project.path() / volume.baked));
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& f : std::filesystem::directory_iterator(project.path() / "Saved" / "Probes")) ++files;
    EXPECT_EQ(files, 1u) << "the first bounce's file is gone";

    auto after = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(renderer_->stats().scene.lightProbeVolumes, 1u);
    const glm::ivec2 near = toPixel(view, {0.0f, 0.0f, -0.8f}, size);  // floor just in front of the wall
    const auto a = pixel(*before, near.x, near.y);
    const auto b = pixel(*after, near.x, near.y);
    EXPECT_GT(int(b.r) - int(b.b), int(a.r) - int(a.b) + 15) << "the red wall tints the floor next to it";

    // Cancelling keeps the volume as it was.
    const std::string kept = volume.baked;
    auto cancelled = bakeLightProbeVolumes(*renderer_, *scene, assets, settings, false, [](float) { return false; });
    EXPECT_FALSE(cancelled.has_value());
    EXPECT_EQ(volume.baked, kept);
    EXPECT_TRUE(std::filesystem::exists(project.path() / kept));
}

TEST_F(RendererTest, VolumetricFogGlowsAroundLights) {
    // Night: a black sky, no ambient light, one point light hanging in thick fog.
    auto scene = std::make_unique<Scene>("Night");
    auto& env = scene->environment();
    env.skyZenithColor = env.skyHorizonColor = env.groundColor = glm::vec3(0.0f);
    env.ambientIntensity = 0.0f;
    env.fogDensity = 0.08f;
    env.fogHeightFalloff = 0.001f;
    env.volumetricFogDistance = 40.0f;
    const auto lamp = scene->createEntity("Lamp");
    auto& light = scene->registry().emplace<LightComponent>(lamp);
    light.type = LightType::Point;
    light.intensity = 40.0f;
    light.range = 12.0f;
    scene->registry().get<TransformComponent>(lamp).position = {0.0f, 2.0f, 0.0f};
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 2.0f, 8.0f});
    cam.lookAt({0.0f, 2.0f, 0.0f});
    const glm::uvec2 size(96, 96);
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    auto analytic = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(analytic.has_value());
    EXPECT_FALSE(renderer_->stats().scene.volumetricFog);
    env.volumetricFog = true;
    auto volumetric = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(volumetric.has_value());
    EXPECT_TRUE(renderer_->stats().scene.volumetricFog);
    const glm::ivec2 near = toPixel(view, {1.0f, 2.0f, 0.0f}, size);
    const glm::ivec2 far = toPixel(view, {0.0f, 2.0f, 0.0f}, size);
    const glm::ivec2 corner(2, 2);
    EXPECT_LT(luminance(pixel(*analytic, near.x, near.y)), 5.0f) << "the simple fog is not lit by the lamp";
    EXPECT_GT(luminance(pixel(*volumetric, far.x, far.y)), 60.0f) << "the fog glows around the lamp";
    EXPECT_GT(luminance(pixel(*volumetric, near.x, near.y)), luminance(pixel(*volumetric, corner.x, corner.y)) + 20.0f)
        << "brightest near it, fading away";
}

TEST(VirtualShadowMap, VisibleSurfacesChooseTheFinePages) {
    VirtualShadowMap vsm;
    FlyCameraController cam;
    cam.setPosition({0.0f, 2.0f, 0.0f});
    cam.setRotation(0.0f, -20.0f);
    const CameraView view = cam.view(16.0f / 9.0f);
    const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f));
    const VsmFrame first = vsm.update(view, sun, 50.0f, {}, 0);
    ASSERT_GE(first.levels, 4);

    // Slots are absolute pages modulo 16 inside each level's window: decoding finds the page.
    std::vector<std::uint32_t> bits(static_cast<std::size_t>(VirtualShadowMap::kPagesPerLevel * VirtualShadowMap::kPagesPerLevel * first.levels), 0u);
    const glm::ivec2 base = first.windowMin[0];
    const glm::ivec2 wanted = base + glm::ivec2(13, 2);
    auto slot = [](int page) { return ((page % 16) + 16) % 16; };
    bits[static_cast<std::size_t>(slot(wanted.y) * 16 + slot(wanted.x))] = 1u;
    const VsmFeedback feedback = VirtualShadowMap::decodeRequests(bits, 16, first.levels, first.windowMin, first.generation);
    ASSERT_EQ(feedback.pages.size(), 1u);
    EXPECT_EQ(feedback.pages[0], glm::ivec3(0, wanted.x, wanted.y));

    const VsmFrame second = vsm.update(view, sun, 50.0f, {}, 0, &feedback);
    EXPECT_TRUE(second.fromFeedback);
    EXPECT_LT(second.requested, first.requested) << "fine levels: only the requested page";
    int fine = 0;
    for (int l = 0; l < second.levels - VirtualShadowMap::kFrustumLevels; ++l) {
        for (int i = 0; i < 256; ++i) {
            fine += second.pageTable[(static_cast<std::size_t>(l * 256 + i)) * 4 + 3] > 0.5f ? 1 : 0;
        }
    }
    EXPECT_GE(fine, 1);

    VsmFeedback stale = feedback;
    stale.generation += 1;
    EXPECT_FALSE(vsm.update(view, sun, 50.0f, {}, 0, &stale).fromFeedback) << "requests from another light space are ignored";
}

TEST_F(RendererTest, VirtualShadowPagesFollowTheVisibleSurfaces) {
    auto scene = shadowScene();
    addCube(*scene, {0.0f, 2.0f, 0.0f});
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({4.0f, 3.0f, 4.0f});
    cam.lookAt({0.0f, 0.0f, -2.0f});
    const CameraView view = cam.view(4.0f / 3.0f);
    RenderSettings settings;
    settings.virtualShadowPageBudget = 0;
    std::vector<CapturedImage> frames;
    std::vector<SceneDrawStats> stats;
    for (int i = 0; i < 4; ++i) {
        auto image = renderer_->capture(*scene, view, {160, 120}, settings);
        ASSERT_TRUE(image.has_value());
        frames.push_back(std::move(*image));
        stats.push_back(renderer_->stats().scene);
    }
    EXPECT_FALSE(stats[0].virtualPagesFromSurfaces) << "nothing read back yet";
    EXPECT_TRUE(stats[3].virtualPagesFromSurfaces);
    EXPECT_LT(stats[3].virtualPagesRequested, stats[0].virtualPagesRequested) << "fewer pages than the whole frustum";
    std::size_t differing = 0;
    for (std::size_t i = 0; i < frames[0].rgba.size(); ++i) differing += std::abs(int(frames[0].rgba[i]) - int(frames[3].rgba[i])) > 3 ? 1 : 0;
    EXPECT_LT(differing, frames[0].rgba.size() / 200) << "the shadows look the same";
}

TEST_F(RendererTest, LocalShadowTilesAreKeptUntilTheirLightOrCastersChange) {
    // Four shadowed point lights far apart (24 tiles), each over its own cube.
    auto scene = std::make_unique<Scene>("Lamps");
    scene->environment().ambientIntensity = 0.0f;
    scene->environment().fogDensity = 0.0f;
    const auto floor = scene->createEntity("Floor");
    auto& fm = scene->registry().emplace<MeshRendererComponent>(floor);
    fm.mesh = MeshPrimitive::Plane;
    fm.baseColor = glm::vec3(0.8f);
    scene->registry().get<TransformComponent>(floor).scale = glm::vec3(60.0f, 1.0f, 60.0f);
    std::vector<entt::entity> cubes;
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 at(static_cast<float>(i % 2) * 20.0f - 10.0f, 0.0f, static_cast<float>(i / 2) * 20.0f - 10.0f);
        const auto cube = scene->createEntity("Cube");
        scene->registry().emplace<MeshRendererComponent>(cube).baseColor = glm::vec3(0.8f);
        scene->registry().get<TransformComponent>(cube).position = at + glm::vec3(0.0f, 1.5f, 0.0f);
        cubes.push_back(cube);
        const auto lamp = scene->createEntity("Lamp");
        auto& light = scene->registry().emplace<LightComponent>(lamp);
        light.type = LightType::Point;
        light.intensity = 40.0f;
        light.range = 8.0f;
        light.castShadows = true;
        scene->registry().get<TransformComponent>(lamp).position = at + glm::vec3(0.0f, 4.0f, 0.0f);
    }
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 40.0f, 20.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    RenderSettings settings;
    settings.bloom = false;
    auto first = renderer_->capture(*scene, view, {128, 128}, settings);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(renderer_->stats().scene.localShadowTiles, 24u) << "more than the 16 tiles of old";
    EXPECT_EQ(renderer_->stats().scene.localShadowTilesDrawn, 24u);

    auto again = renderer_->capture(*scene, view, {128, 128}, settings);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(renderer_->stats().scene.localShadowTilesDrawn, 0u) << "nothing changed: every tile cached";
    EXPECT_EQ(first->rgba, again->rgba);

    // One cube moves under its lamp: only that lamp's faces that see it are drawn again.
    scene->registry().get<TransformComponent>(cubes[0]).position.x += 1.0f;
    scene->updateWorldTransforms();
    auto moved = renderer_->capture(*scene, view, {128, 128}, settings);
    ASSERT_TRUE(moved.has_value());
    EXPECT_GT(renderer_->stats().scene.localShadowTilesDrawn, 0u);
    EXPECT_LE(renderer_->stats().scene.localShadowTilesDrawn, 6u);
    EXPECT_NE(first->rgba, moved->rgba) << "its shadow moved";
}

#include "renderer/TextureCompression.hpp"
#include "renderer/TextureCache.hpp"

TEST(TextureCompression, BlockFormatsMipChainsAndCacheFiles) {
    std::vector<std::uint8_t> rgba(64 * 32 * 4);
    for (std::size_t i = 0; i < 64 * 32; ++i) {
        rgba[i * 4] = static_cast<std::uint8_t>(i % 64 * 4);
        rgba[i * 4 + 1] = 128;
        rgba[i * 4 + 2] = 40;
        rgba[i * 4 + 3] = 255;
    }
    const TextureMips color = prepareTexture(rgba, 64, 32, TextureKind::Color, true);
    EXPECT_EQ(color.format, nvrhi::Format::BC1_UNORM_SRGB) << "opaque colour: BC1";
    ASSERT_EQ(color.levels.size(), 7u);  // 64x32 .. 1x1
    EXPECT_EQ(color.levels[0].size(), 16u * 8u * 8u) << "4x4 blocks of 8 bytes";
    EXPECT_EQ(color.levels.back().size(), 8u) << "a 1x1 level is one block";
    EXPECT_EQ(prepareTexture(rgba, 64, 32, TextureKind::Normal, true).format, nvrhi::Format::BC5_UNORM);
    EXPECT_EQ(prepareTexture(rgba, 64, 32, TextureKind::Color, false).format, nvrhi::Format::SRGBA8_UNORM);
    rgba[3] = 0;
    EXPECT_EQ(prepareTexture(rgba, 64, 32, TextureKind::Color, true).format, nvrhi::Format::BC3_UNORM_SRGB) << "with alpha: BC3";

    // A flat colour survives BC1 nearly exactly (decode the block's two 5:6:5 endpoints).
    std::vector<std::uint8_t> flat(4 * 4 * 4);
    for (std::size_t i = 0; i < 16; ++i) {
        flat[i * 4] = 200;
        flat[i * 4 + 1] = 100;
        flat[i * 4 + 2] = 50;
        flat[i * 4 + 3] = 255;
    }
    const auto block = compressBlocks(flat, 4, 4, nvrhi::Format::BC1_UNORM);
    ASSERT_EQ(block.size(), 8u);
    const unsigned c0 = block[0] | (block[1] << 8);
    EXPECT_NEAR(static_cast<int>((c0 >> 11) * 255 / 31), 200, 9);
    EXPECT_NEAR(static_cast<int>(((c0 >> 5) & 63) * 255 / 63), 100, 5);

    test::TempDir dir;
    const auto file = dir.path() / "Saved" / "TextureCache" / "a.ltex";
    ASSERT_TRUE(saveTextureMips(file, color).has_value());
    auto loaded = loadTextureMips(file);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->format, color.format);
    EXPECT_EQ(loaded->levels, color.levels);
    std::filesystem::resize_file(file, 100);
    EXPECT_FALSE(loadTextureMips(file).has_value());
}

TEST_F(RendererTest, TexturesAreCompressedCachedAndStreamed) {
    if (!context_->caps().textureCompressionBC) GTEST_SKIP() << "no BC texture support";
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Textures");
    std::vector<std::uint8_t> pixels(1024 * 1024 * 3);
    for (std::size_t i = 0; i < 1024 * 1024; ++i) {
        pixels[i * 3] = static_cast<std::uint8_t>((i % 1024) / 4);
        pixels[i * 3 + 1] = static_cast<std::uint8_t>((i / 1024) / 4);
        pixels[i * 3 + 2] = 90;
    }
    ASSERT_NE(stbi_write_png((project.path() / "Content" / "Textures" / "big.png").string().c_str(), 1024, 1024, 3,
                             pixels.data(), 1024 * 3), 0);
    AssetLibrary assets(project.path());
    TextureCache& cache = context_->textures();
    cache.beginFrame();
    // Far away: only the small mips are made resident.
    const std::uint32_t far = cache.index(assets, "Content/Textures/big.png", TextureKind::Color, true, 64.0f);
    ASSERT_NE(far, UINT32_MAX);
    const auto small = cache.stats();
    EXPECT_GE(small.compressed, 1u);
    EXPECT_LT(small.residentBytes, std::size_t{1} << 20) << "64 texels across, not the whole 1024 chain";
    std::size_t cacheFiles = 0;
    for ([[maybe_unused]] const auto& f : std::filesystem::directory_iterator(project.path() / "Saved" / "TextureCache")) ++cacheFiles;
    EXPECT_EQ(cacheFiles, 1u) << "the compressed chain is kept for the next run";

    // Close up: the next frame streams in the full resolution, under a new bindless index.
    EXPECT_EQ(cache.index(assets, "Content/Textures/big.png", TextureKind::Color, true, 4096.0f), far);
    cache.beginFrame();
    EXPECT_EQ(cache.stats().streamedIn, 1u);
    const std::uint32_t near = cache.index(assets, "Content/Textures/big.png", TextureKind::Color, true, 4096.0f);
    EXPECT_NE(near, far);
    EXPECT_GT(cache.stats().residentBytes, std::size_t{512} << 10) << "1024x1024 BC1 is 512 KiB plus mips";

    // Over budget: the finest mips go.
    cache.setBudget(std::size_t{200} << 10);
    cache.beginFrame();
    EXPECT_GE(cache.stats().reduced, 1u);
    EXPECT_LE(cache.stats().residentBytes, std::size_t{200} << 10);
    cache.setBudget(std::size_t{1536} << 20);
    context_->gpu()->waitForIdle();
    cache.invalidateFile((std::filesystem::weakly_canonical(project.path() / "Content" / "Textures" / "big.png")).generic_string());
}

TEST_F(RendererTest, AutoExposureMatchesBrightAndDimScenesAndAdaptsOverTime) {
    auto scene = shadowScene();
    addCube(*scene, {0.0f, 1.0f, 0.0f});
    scene->environment().fogDensity = 0.0f;
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 4.0f, 8.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    auto& env = scene->environment();
    auto setLight = [&](float scale) {
        for (auto [e, light] : scene->registry().view<LightComponent>().each()) light.intensity = 3.0f * scale;
        env.ambientIntensity = 0.5f * scale;
        env.skyZenithColor = glm::vec3(0.07f, 0.20f, 0.56f) * scale;
        env.skyHorizonColor = glm::vec3(0.56f, 0.67f, 0.82f) * scale;
    };
    auto average = [&](const CapturedImage& image) {
        double sum = 0.0;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) sum += luminance({image.rgba[i], image.rgba[i + 1], image.rgba[i + 2], 255});
        return sum / static_cast<double>(image.rgba.size() / 4);
    };
    RenderSettings settings;
    settings.bloom = false;
    auto shoot = [&](float scale, const RenderSettings& s) {
        setLight(scale);
        auto image = renderer_->capture(*scene, view, {64, 64}, s);
        EXPECT_TRUE(image.has_value());
        return image ? average(*image) : 0.0;
    };
    const double fixedDim = shoot(0.25f, settings), fixedBright = shoot(4.0f, settings);
    EXPECT_GT(fixedBright, fixedDim + 60.0) << "a fixed exposure shows the difference";
    env.autoExposure = true;
    env.exposure = 1.0f;
    const double autoDim = shoot(0.25f, settings);
    EXPECT_TRUE(renderer_->stats().scene.autoExposure);
    const double autoBright = shoot(4.0f, settings);
    EXPECT_LT(std::abs(autoBright - autoDim), 25.0) << "automatic exposure brings both to a similar brightness";
    EXPECT_GT(autoDim, fixedDim + 20.0) << "the dim scene is brightened";

    // A live view adapts gradually: adapted to the bright scene, the first dim frame is still dark.
    shoot(4.0f, settings);
    RenderSettings live = settings;
    live.temporalAntiAliasing = true;
    const double jumped = shoot(0.25f, live);
    EXPECT_LT(jumped, autoDim - 25.0) << "not adapted yet";
}

TEST_F(RendererTest, MotionBlurSmearsMovingObjectsOnly) {
    entt::entity cube = entt::null;
    auto scene = edgeScene(&cube);
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 3.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    RenderSettings live;
    live.antiAliasing = false;
    live.bloom = false;
    live.shadows = false;
    live.temporalAntiAliasing = true;  // a live view: motion vectors from the previous frame
    // The cube slides to the right, 0.12 m a frame; the last frame is compared.
    auto run = [&](float blur) {
        scene->environment().motionBlur = blur;
        std::optional<CapturedImage> last;
        for (int i = 0; i < 6; ++i) {
            scene->registry().get<TransformComponent>(cube).position.x = -0.4f + 0.12f * static_cast<float>(i);
            scene->updateWorldTransforms();
            auto f = renderer_->capture(*scene, view, {96, 96}, live);
            EXPECT_TRUE(f.has_value());
            if (f) last = std::move(*f);
        }
        return last;
    };
    const auto sharp = run(0.0f);
    EXPECT_FALSE(renderer_->stats().scene.motionBlur);
    const auto blurred = run(1.0f);
    EXPECT_TRUE(renderer_->stats().scene.motionBlur);
    ASSERT_TRUE(sharp && blurred);
    EXPECT_GT(partialPixels(*blurred), partialPixels(*sharp) + 20) << "the moving edges are smeared";

    // Still: nothing moves, nothing blurs.
    scene->environment().motionBlur = 1.0f;
    std::optional<CapturedImage> still;
    for (int i = 0; i < 4; ++i) {
        auto f = renderer_->capture(*scene, view, {96, 96}, live);
        ASSERT_TRUE(f.has_value());
        still = std::move(*f);
    }
    scene->environment().motionBlur = 0.0f;
    auto reference = renderer_->capture(*scene, view, {96, 96}, live);
    ASSERT_TRUE(reference.has_value());
    // (Consecutive TAA frames differ a little anyway: compare how much edge there is.)
    EXPECT_NEAR(partialPixels(*still), partialPixels(*reference), 8) << "a still image stays sharp";
}

TEST_F(RendererTest, ContactShadowsDarkenWhereObjectsMeetTheGround) {
    auto scene = shadowScene();
    for (int i = 0; i < 5; ++i) {
        const auto e = addCube(*scene, {-2.0f + static_cast<float>(i), 0.15f, 0.0f});
        scene->registry().get<TransformComponent>(e).scale = glm::vec3(0.3f);
    }
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 1.5f, 3.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView view = cam.view(4.0f / 3.0f);
    RenderSettings s;
    s.bloom = false;
    s.virtualShadows = false;    // coarse cascades: what contact shadows are for
    s.shadowResolution = 256;
    s.contactShadows = false;
    auto without = renderer_->capture(*scene, view, {160, 120}, s);
    ASSERT_TRUE(without.has_value());
    EXPECT_FALSE(renderer_->stats().scene.contactShadows);
    s.contactShadows = true;
    auto with = renderer_->capture(*scene, view, {160, 120}, s);
    ASSERT_TRUE(with.has_value());
    EXPECT_TRUE(renderer_->stats().scene.contactShadows);
    int darker = 0, brighter = 0;
    for (std::uint32_t y = 0; y < 120; ++y) {
        for (std::uint32_t x = 0; x < 160; ++x) {
            const float a = luminance(pixel(*without, x, y)), b = luminance(pixel(*with, x, y));
            darker += b < a - 8.0f ? 1 : 0;
            brighter += b > a + 8.0f ? 1 : 0;
        }
    }
    EXPECT_GT(darker, 20) << "contact shadows add shade";
    EXPECT_EQ(brighter, 0) << "and never light";
    EXPECT_LT(darker, 160 * 120 / 10) << "only near the objects";
}

// ---- glTF material extensions -----------------------------------------------------------------

namespace {

/// Renders the imported cube with `materialJson` in front of an unlit red wall, seen head on.
std::optional<CapturedImage> renderExtensionCube(Renderer& renderer, const std::string& materialJson, const std::string& textures = "",
                                                 bool wall = true, glm::uvec2 size = {64, 64}) {
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Models");
    test::writeCubeGltfWithMaterial(project.path() / "Content" / "Models" / "cube.gltf", materialJson, textures);
    AssetLibrary assets(project.path());
    auto model = assets.model(project.path() / "Content" / "Models" / "cube.gltf");
    if (!model) return std::nullopt;
    auto scene = std::make_unique<Scene>("Test");
    scene->environment().fogDensity = 0.0f;
    const auto sun = scene->createEntity("Sun");
    scene->registry().get<TransformComponent>(sun).rotationDeg = {-30.0f, 20.0f, 0.0f};
    scene->registry().emplace<LightComponent>(sun).intensity = 3.0f;
    const entt::entity root = instantiateModel(*scene, **model, "Content/Models/cube.gltf", "Cube");
    auto& rt = scene->registry().get<TransformComponent>(root);
    rt.position = {0.0f, -1.0f, 0.0f};  // the node lifts it by 1; the node also scales it by 2
    if (wall) {
        const auto w = scene->createEntity("Wall");
        auto& m = scene->registry().emplace<MeshRendererComponent>(w);
        m.mesh = MeshPrimitive::Cube;
        m.baseColor = glm::vec3(0.0f);
        m.emissive = glm::vec3(1.0f, 0.0f, 0.0f);
        m.emissiveStrength = 1.0f;
        auto& t = scene->registry().get<TransformComponent>(w);
        t.position = {0.0f, 0.0f, -4.0f};
        t.scale = {20.0f, 20.0f, 0.2f};
    }
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.bloom = false;
    settings.ambientOcclusion = false;
    settings.temporalAntiAliasing = false;
    auto image = renderer.capture(*scene, cam.view(1.0f), {size.x, size.y}, settings);
    if (!image) return std::nullopt;
    return std::move(*image);
}


} // namespace

TEST_F(RendererTest, TransmissiveGlassShowsTheSceneBehindTintedByItsVolume) {
    const std::string white = R"({"name": "Glass", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.05})";
    auto opaque = renderExtensionCube(*renderer_, white + "}");
    ASSERT_TRUE(opaque.has_value());
    auto glass = renderExtensionCube(*renderer_, white + R"(, "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1}}})");
    ASSERT_TRUE(glass.has_value());
    EXPECT_EQ(renderer_->stats().scene.transparentObjects, 1u) << "drawn with the see-through surfaces";
    auto tinted = renderExtensionCube(*renderer_, white + R"(, "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1},
        "KHR_materials_volume": {"thicknessFactor": 1, "attenuationDistance": 1, "attenuationColor": [0.05, 0.05, 1]}}})");
    ASSERT_TRUE(tinted.has_value());
    const auto o = pixel(*opaque, 32, 32);
    const auto g = pixel(*glass, 32, 32);
    const auto t = pixel(*tinted, 32, 32);
    EXPECT_GT(o.g, 60) << "the opaque white cube hides the red wall";
    EXPECT_GT(g.r, 120) << "the red wall shows through the glass";
    EXPECT_GT(g.r, g.g + 60);
    EXPECT_LT(t.r, g.r * 0.6f) << "a volume as thick as the distance that keeps 5% of red light darkens it";
    EXPECT_GT(t.r, 3) << "but not black";
}

TEST_F(RendererTest, TextureTransformPicksPartOfTheMap) {
    // Left half red, right half green (the sky behind is blue).
    std::vector<unsigned char> rgba(8 * 8 * 4);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            unsigned char* p = &rgba[static_cast<std::size_t>(y * 8 + x) * 4];
            p[0] = x < 4 ? 230 : 10;
            p[1] = x < 4 ? 10 : 230;
            p[2] = 10;
            p[3] = 255;
        }
    }
    const std::string textures = R"("textures": [{"source": 0, "sampler": 0}], "samplers": [{"magFilter": 9728, "minFilter": 9728}],
        "images": [{"uri": "data:image/png;base64,)" + test::base64(test::encodePng(8, 8, rgba)) + R"("}],)";
    auto count = [](const CapturedImage& image) {
        int red = 0, green = 0;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            const int r = image.rgba[i], g = image.rgba[i + 1], b = image.rgba[i + 2];
            if (r > g + 40 && r > b + 40) ++red;
            if (g > r + 40 && g > b + 40) ++green;
        }
        return std::pair(red, green);
    };
    auto plain = renderExtensionCube(*renderer_, R"({"name": "Map", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0}})",
                                     textures, false);
    auto left = renderExtensionCube(*renderer_, R"({"name": "Map", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0,
        "extensions": {"KHR_texture_transform": {"scale": [0.45, 1]}}}, "metallicFactor": 0}})", textures, false);
    auto right = renderExtensionCube(*renderer_, R"({"name": "Map", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0,
        "extensions": {"KHR_texture_transform": {"offset": [0.55, 0], "scale": [0.45, 1]}}}, "metallicFactor": 0}})", textures, false);
    ASSERT_TRUE(plain && left && right);
    const auto [plainRed, plainGreen] = count(*plain);
    const auto [leftRed, leftGreen] = count(*left);
    const auto [rightRed, rightGreen] = count(*right);
    EXPECT_GT(plainRed, 50);
    EXPECT_GT(plainGreen, 50) << "untransformed: both halves";
    EXPECT_GT(leftRed, plainRed);
    EXPECT_LT(leftGreen, 5) << "scaled into the left half: red only";
    EXPECT_LT(rightRed, 5) << "offset into the right half: green only";
    EXPECT_GT(rightGreen, plainGreen);
}

TEST_F(RendererTest, ClearcoatSheenAndSpecularChangeTheReflection) {
    auto render = [&](const std::string& extensions) {
        auto image = renderExtensionCube(*renderer_, R"({"name": "Dark", "pbrMetallicRoughness": {"baseColorFactor": [0.03, 0.03, 0.03, 1],
            "metallicFactor": 0, "roughnessFactor": 0.9})" + extensions + "}", "", false);
        EXPECT_TRUE(image.has_value());
        float sum = 0.0f;
        for (std::uint32_t y = 20; y < 44; ++y) {
            for (std::uint32_t x = 20; x < 44; ++x) sum += luminance(pixel(*image, x, y));
        }
        return sum / (24.0f * 24.0f);
    };
    const float plain = render("");
    const float coated = render(R"(, "extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 1, "clearcoatRoughnessFactor": 0.05}})");
    const float velvet = render(R"(, "extensions": {"KHR_materials_sheen": {"sheenColorFactor": [1, 1, 1], "sheenRoughnessFactor": 0.5}})");
    const float noSpecular = render(R"(, "extensions": {"KHR_materials_specular": {"specularFactor": 0}})");
    EXPECT_GT(coated, plain + 2.0f) << "a clear coat reflects the sky";
    EXPECT_GT(velvet, plain + 2.0f) << "sheen brightens the dark cloth";
    EXPECT_LT(noSpecular, plain - 0.5f) << "no dielectric reflection at all";
}

TEST_F(RendererTest, MorphTargetsMoveTheMeshAndTheSecondUvSetPicksItsTexels) {
    test::TempDir project;
    test::writeMorphQuadGltf(project.path() / "Content" / "Models" / "quad.gltf");
    AssetLibrary assets(project.path());
    auto model = assets.model(project.path() / "Content" / "Models" / "quad.gltf");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const entt::entity root = instantiateModel(*scene, **model, "Content/Models/quad.gltf", "Quad");
    entt::entity quad = entt::null;
    for (auto [e, m] : scene->registry().view<MorphWeightsComponent>().each()) quad = e;
    ASSERT_TRUE(quad != entt::null);
    (void)root;
    FlyCameraController cam;
    cam.setPosition({0.5f, 0.0f, 3.0f});
    cam.lookAt({0.5f, 0.0f, 0.0f});
    const CameraView view = cam.view(1.0f);
    const glm::uvec2 size(96, 96);
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.temporalAntiAliasing = false;
    auto render = [&](float weight) {
        scene->registry().get<MorphWeightsComponent>(quad).weights = {weight};
        scene->updateWorldTransforms();
        auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
        EXPECT_TRUE(image.has_value());
        return std::move(*image);
    };
    auto green = [](glm::u8vec4 p) { return p.g > p.r + 60 && p.g > p.b + 40; };
    const glm::ivec2 left = toPixel(view, {-0.3f, 0.0f, 0.0f}, size);
    const glm::ivec2 right = toPixel(view, {1.3f, 0.0f, 0.0f}, size);
    const CapturedImage rest = render(0.0f);
    EXPECT_EQ(renderer_->stats().scene.morphedObjects, 0u) << "zero weights: the mesh as it is";
    EXPECT_TRUE(green(pixel(rest, left.x, left.y))) << "the second UV set shows only the map's green half";
    EXPECT_FALSE(green(pixel(rest, right.x, right.y)));
    const CapturedImage moved = render(1.0f);
    EXPECT_EQ(renderer_->stats().scene.morphedObjects, 1u);
    EXPECT_FALSE(green(pixel(moved, left.x, left.y))) << "the target slid it 1 m to the right";
    EXPECT_TRUE(green(pixel(moved, right.x, right.y)));
}

TEST_F(RendererTest, MaterialAssetsCarryAlphaSidesAndLayers) {
    // A model whose own material is cut out and two-sided; a .lmat on it replaces all that.
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Models");
    test::writeCubeGltf(project.path() / "Content" / "Models" / "cube.gltf", false);
    AssetLibrary assets(project.path());
    auto model = assets.model(project.path() / "Content" / "Models" / "cube.gltf");
    ASSERT_TRUE(model.has_value());
    const auto file = project.path() / "Content" / "Materials" / "Glass.lmat";
    MaterialAsset glass;
    glass.baseColor = glm::vec3(1.0f);
    glass.roughness = 0.05f;
    ASSERT_TRUE(saveMaterial(glass, file).has_value());

    auto scene = sceneWithSun();
    scene->environment().fogDensity = 0.0f;
    const entt::entity root = instantiateModel(*scene, **model, "Content/Models/cube.gltf", "Cube");
    scene->registry().get<TransformComponent>(root).position = {0.0f, -1.0f, 0.0f};
    for (auto [e, m] : scene->registry().view<MeshRendererComponent>().each()) m.material = "Content/Materials/Glass.lmat";
    const auto wall = scene->createEntity("Wall");
    auto& wm = scene->registry().emplace<MeshRendererComponent>(wall);
    wm.baseColor = glm::vec3(0.0f);
    wm.emissive = {1.0f, 0.0f, 0.0f};
    wm.emissiveStrength = 1.0f;
    auto& wt = scene->registry().get<TransformComponent>(wall);
    wt.position = {0.0f, 0.0f, -4.0f};
    wt.scale = {20.0f, 20.0f, 0.2f};
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.temporalAntiAliasing = false;
    auto render = [&](const MaterialAsset& m) {
        EXPECT_TRUE(saveMaterial(m, file).has_value());
        assets.reloadMaterial("Content/Materials/Glass.lmat");
        auto image = renderer_->capture(*scene, cam.view(1.0f), {64, 64}, settings);
        EXPECT_TRUE(image.has_value());
        return pixel(*image, 32, 32);
    };
    const auto opaque = render(glass);
    EXPECT_EQ(renderer_->stats().scene.transparentObjects, 0u);
    EXPECT_GT(opaque.g, 60) << "opaque white";

    MaterialAsset seeThrough = glass;
    seeThrough.alphaMode = MaterialAsset::AlphaMode::Transparent;
    seeThrough.opacity = 0.3f;
    const auto blended = render(seeThrough);
    EXPECT_EQ(renderer_->stats().scene.transparentObjects, 1u);
    EXPECT_GT(blended.r, blended.g + 40) << "the red wall shows through";

    MaterialAsset clear = glass;
    clear.transmission = 1.0f;
    const auto transmitted = render(clear);
    EXPECT_EQ(renderer_->stats().scene.transparentObjects, 1u) << "transmission draws it over the scene behind";
    EXPECT_GT(transmitted.r, transmitted.g + 60);

    MaterialAsset tinted = clear;
    tinted.thickness = 1.0f;
    tinted.attenuationDistance = 1.0f;
    tinted.attenuationColor = {0.05f, 0.05f, 1.0f};
    const auto absorbed = render(tinted);
    EXPECT_LT(absorbed.r, transmitted.r * 0.6f) << "absorbed in the volume";
}

// ---- Material graphs --------------------------------------------------------------------------

#include "assets/MaterialGraph.hpp"

#include <nlohmann/json.hpp>

namespace {

/// A sphere wearing `material` (project-relative key), lit by the sun, filling the picture.
std::optional<CapturedImage> renderSphere(Renderer& renderer, AssetLibrary& assets, const std::string& material,
                                          glm::vec3 position = glm::vec3(0.0f), bool wall = false) {
    auto scene = std::make_unique<Scene>("Sphere");
    scene->environment().fogDensity = 0.0f;
    const auto sun = scene->createEntity("Sun");
    scene->registry().get<TransformComponent>(sun).rotationDeg = {-40.0f, -35.0f, 0.0f};
    scene->registry().emplace<LightComponent>(sun).intensity = 3.0f;
    const auto ball = scene->createEntity("Ball");
    auto& mesh = scene->registry().emplace<MeshRendererComponent>(ball);
    mesh.mesh = MeshPrimitive::Sphere;
    mesh.material = material;
    scene->registry().get<TransformComponent>(ball).position = position;
    if (wall) {
        const auto w = scene->createEntity("Wall");
        auto& wm = scene->registry().emplace<MeshRendererComponent>(w);
        wm.baseColor = glm::vec3(0.0f);
        wm.emissive = {0.0f, 0.0f, 1.0f};
        wm.emissiveStrength = 1.0f;
        auto& t = scene->registry().get<TransformComponent>(w);
        t.position = {0.0f, 0.0f, -3.0f};
        t.scale = {20.0f, 20.0f, 0.2f};
    }
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 0.0f, 1.6f});
    cam.lookAt({0.0f, 0.0f, 0.0f});
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.temporalAntiAliasing = false;
    settings.bloom = false;
    auto image = renderer.capture(*scene, cam.view(1.0f), {96, 96}, settings);
    if (!image) return std::nullopt;
    return std::move(*image);
}

nlohmann::json graphNode(int id, std::string_view type, nlohmann::json inputs = nlohmann::json::object()) {
    return {{"id", id}, {"type", std::string(type)}, {"x", 0}, {"y", 0}, {"inputs", std::move(inputs)}};
}
nlohmann::json graphLink(int from, std::string_view fromPin, int to, std::string_view toPin) {
    return {{"from", from}, {"fromPin", std::string(fromPin)}, {"to", to}, {"toPin", std::string(toPin)}};
}

} // namespace

TEST_F(RendererTest, MaterialGraphTextureTimesColourMatchesTheSameMaterialAsset) {
    test::TempDir project;
    // A texture with a soft gradient and a hole (alpha 0 in the top-left quarter).
    std::vector<unsigned char> rgba(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            unsigned char* p = &rgba[static_cast<std::size_t>(y * 16 + x) * 4];
            p[0] = static_cast<unsigned char>(100 + x * 9);
            p[1] = static_cast<unsigned char>(200 - y * 6);
            p[2] = 120;
            p[3] = x < 8 && y < 8 ? 0 : 255;
        }
    }
    std::filesystem::create_directories(project.path() / "Content" / "Textures");
    const auto png = test::encodePng(16, 16, rgba);
    std::ofstream(project.path() / "Content" / "Textures" / "grad.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    MaterialAsset lmat;
    lmat.baseColor = {1.0f, 0.5f, 0.25f};
    lmat.roughness = 0.6f;
    lmat.baseColorTexture = "Content/Textures/grad.png";
    lmat.alphaMode = MaterialAsset::AlphaMode::CutOut;
    lmat.alphaCutoff = 0.5f;
    ASSERT_TRUE(saveMaterial(lmat, project.path() / "Content" / "Materials" / "Plain.lmat").has_value());
    const nlohmann::json graph{
        {"version", 1},
        {"nodes", {graphNode(1, "output.material", {{"roughness", 0.6}, {"metallic", 0.0}, {"alphaMode", 1}, {"alphaCutoff", 0.5}}),
                   graphNode(2, "texture.color", {{"texture", "Content/Textures/grad.png"}}),
                   graphNode(3, "const.color", {{"color", {1.0, 0.5, 0.25}}}), graphNode(4, "math.multiply")}},
        {"links", {graphLink(2, "rgb", 4, "a"), graphLink(3, "color", 4, "b"), graphLink(4, "result", 1, "baseColor"),
                   graphLink(2, "a", 1, "opacity")}}};
    std::ofstream(project.path() / "Content" / "Materials" / "Graph.lmgraph") << graph.dump(2);
    AssetLibrary assets(project.path());
    auto plain = renderSphere(*renderer_, assets, "Content/Materials/Plain.lmat");
    auto graphed = renderSphere(*renderer_, assets, "Content/Materials/Graph.lmgraph");
    ASSERT_TRUE(plain && graphed);
    double difference = 0.0;
    for (std::size_t i = 0; i < plain->rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) difference += std::abs(int(plain->rgba[i + c]) - int(graphed->rgba[i + c]));
    }
    difference /= static_cast<double>(plain->rgba.size() / 4 * 3);
    EXPECT_LT(difference, 1.5) << "the graph draws what the material asset with the same settings draws";
    // Not trivially equal: the sphere covers the middle, tinted orange.
    const auto middle = pixel(*graphed, 48, 48);
    EXPECT_GT(middle.r, middle.b + 40);

    // Cut out everywhere (opacity 0 below the cutoff): the sphere is gone, in the depth
    // prepass too (the sky shows, not a hole in black).
    const nlohmann::json gone{{"version", 1},
                              {"nodes", {graphNode(1, "output.material", {{"alphaMode", 1}, {"opacity", 0.0}})}},
                              {"links", nlohmann::json::array()}};
    std::ofstream(project.path() / "Content" / "Materials" / "Gone.lmgraph") << gone.dump(2);
    auto empty = renderSphere(*renderer_, assets, "Content/Materials/Gone.lmgraph");
    ASSERT_TRUE(empty.has_value());
    const auto sky = pixel(*empty, 48, 48);
    EXPECT_GT(sky.b, sky.r) << "the sky behind";
    EXPECT_GT(int(sky.r) + sky.g + sky.b, 200) << "not a black hole";
}

TEST_F(RendererTest, MaterialGraphWorldOffsetMovesTheMeshInEveryPass) {
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Materials");
    const nlohmann::json graph{
        {"version", 1},
        {"nodes", {graphNode(1, "output.material", {{"baseColor", {1.0, 0.1, 0.1}}, {"roughness", 0.9}}),
                   graphNode(2, "vector.combine", {{"x", 0.9}, {"y", 0.0}, {"z", 0.0}})}},
        {"links", {graphLink(2, "vector", 1, "worldOffset")}}};
    std::ofstream(project.path() / "Content" / "Materials" / "Moved.lmgraph") << graph.dump(2);
    AssetLibrary assets(project.path());
    auto moved = renderSphere(*renderer_, assets, "Content/Materials/Moved.lmgraph", glm::vec3(0.0f), true);
    ASSERT_TRUE(moved.has_value());
    // The sphere (radius 0.5) now sits 0.9 m to the right: the middle shows the blue wall, the
    // right of the picture the red sphere, drawn (the depth prepass moved it the same way).
    const auto middle = pixel(*moved, 48, 48);
    EXPECT_GT(middle.b, middle.r + 40) << "nothing left in the middle";
    int red = 0;
    for (std::uint32_t y = 30; y < 66; ++y) {
        for (std::uint32_t x = 72; x < 96; ++x) {
            const auto p = pixel(*moved, x, y);
            if (p.r > p.b + 40) ++red;
        }
    }
    EXPECT_GT(red, 200) << "the sphere, moved and shaded";
}

TEST_F(RendererTest, HudPicturesFromProjectTexturesAndCyrillicText) {
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Textures");
    const auto png = test::solidPng(8, 8, 40, 220, 60);
    std::ofstream(project.path() / "Content" / "Textures" / "icon.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    AssetLibrary assets(project.path());
    auto scene = sceneWithSun();
    scene->updateWorldTransforms();
    FlyCameraController cam;
    cam.setPosition({0.0f, 1.0f, 5.0f});
    HudCanvas hud;
    hud.screenSize = {200.0f, 100.0f};
    hud.addRect({0.0f, 0.0f}, {200.0f, 100.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    hud.addImage({10.0f, 10.0f}, {40.0f, 40.0f}, "Content/Textures/icon.png");
    hud.addImage({60.0f, 10.0f}, {40.0f, 40.0f}, "Content/Textures/icon.png", {1.0f, 0.0f, 1.0f, 1.0f});  // tinted: green x magenta = black
    hud.addText({110.0f, 20.0f}, 40.0f, {1.0f, 1.0f, 1.0f, 1.0f}, "Жд");
    RenderSettings settings;
    settings.bloom = false;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.hud = &hud;
    auto image = renderer_->capture(*scene, cam.view(2.0f), {200, 100}, settings);
    ASSERT_TRUE(image.has_value());
    const auto picture = pixel(*image, 30, 30);
    EXPECT_GT(picture.g, 180) << "the picture's green";
    EXPECT_LT(picture.r, 80);
    const auto tinted = pixel(*image, 80, 30);
    EXPECT_LT(int(tinted.g) + tinted.r + tinted.b, 120) << "tinted by the colour";
    int glyphPixels = 0;
    for (std::uint32_t y = 15; y < 75; ++y) {
        for (std::uint32_t x = 108; x < 200; ++x) {
            if (luminance(pixel(*image, x, y)) > 200.0f) ++glyphPixels;
        }
    }
    EXPECT_GT(glyphPixels, 80) << "Cyrillic glyphs drawn";
}

TEST_F(RendererTest, SeeThroughLeavesCastCutOutShadows) {
    // Blended leaf cards cast a shadow where their map makes them at least half opaque; glass
    // (one opacity, no map) still casts none. Off screen, cut-out cards keep their holes.
    test::TempDir project;
    std::filesystem::create_directories(project.path() / "Content" / "Textures");
    for (const auto& [name, alpha] : {std::pair("solid.png", 255), std::pair("clear.png", 0)}) {
        std::vector<std::uint8_t> rgba(8 * 8 * 4);
        for (std::size_t i = 0; i < 8 * 8; ++i) {
            rgba[i * 4] = 40;
            rgba[i * 4 + 1] = 160;
            rgba[i * 4 + 2] = 40;
            rgba[i * 4 + 3] = static_cast<std::uint8_t>(alpha);
        }
        ASSERT_NE(stbi_write_png((project.path() / "Content" / "Textures" / name).string().c_str(), 8, 8, 4, rgba.data(), 8 * 4), 0);
    }
    AssetLibrary assets(project.path());
    auto scene = shadowScene();
    auto card = [&](glm::vec3 position, float scale, const std::string& map, float opacity, float cutoff) {
        const auto e = scene->createEntity("Card");
        auto& mesh = scene->registry().emplace<MeshRendererComponent>(e);
        mesh.mesh = MeshPrimitive::Plane;
        mesh.baseColorTexture = map;
        mesh.opacity = opacity;
        mesh.alphaCutoff = cutoff;
        auto& transform = scene->registry().get<TransformComponent>(e);
        transform.position = position;
        transform.scale = glm::vec3(scale, 1.0f, scale);
        return e;
    };
    card({-3.0f, 2.0f, 0.0f}, 1.5f, "Content/Textures/solid.png", 0.9f, -1.0f);  // blended, solid map
    card({0.0f, 2.0f, 0.0f}, 1.5f, "Content/Textures/clear.png", 0.9f, -1.0f);   // blended, clear map
    card({3.0f, 2.0f, 0.0f}, 1.5f, "", 0.9f, -1.0f);                             // glass
    scene->updateWorldTransforms();
    const glm::uvec2 size(160, 120);
    const glm::vec3 shadowOffset(0.0f, -2.0f, -2.0f / std::tan(glm::radians(60.0f)));
    FlyCameraController cam;
    cam.setPosition({0.0f, 5.0f, 5.0f});
    cam.lookAt({0.0f, 0.0f, -1.0f});
    const CameraView view = cam.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    auto darkness = [&](const CapturedImage& image, glm::vec3 caster) {
        const glm::ivec2 shadow = toPixel(view, caster + shadowOffset, size);
        const glm::ivec2 lit = toPixel(view, glm::vec3(caster.x, 0.0f, 2.5f), size);
        return luminance(pixel(image, shadow.x, shadow.y)) / std::max(luminance(pixel(image, lit.x, lit.y)), 1.0f);
    };
    for (const bool virtualShadows : {false, true}) {
        RenderSettings settings;
        settings.assets = &assets;
        settings.blockOnAssets = true;
        settings.virtualShadows = virtualShadows;
        settings.virtualShadowPageBudget = 0;
        auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
        ASSERT_TRUE(image.has_value()) << image.error().message;
        EXPECT_EQ(renderer_->stats().scene.transparentObjects, 3u);
        EXPECT_LT(darkness(*image, {-3.0f, 2.0f, 0.0f}), 0.6f) << "solid leaf, virtual " << virtualShadows;
        EXPECT_GT(darkness(*image, {0.0f, 2.0f, 0.0f}), 0.85f) << "clear leaf, virtual " << virtualShadows;
        EXPECT_GT(darkness(*image, {3.0f, 2.0f, 0.0f}), 0.85f) << "glass, virtual " << virtualShadows;
    }

    // A cut-out card above and behind the camera: its map still cuts its shadow.
    auto offscreen = shadowScene();
    const float height = 12.0f;
    const float reach = height / std::tan(glm::radians(60.0f));
    const auto e = offscreen->createEntity("Card");
    auto& mesh = offscreen->registry().emplace<MeshRendererComponent>(e);
    mesh.mesh = MeshPrimitive::Plane;
    mesh.baseColorTexture = "Content/Textures/clear.png";
    mesh.alphaCutoff = 0.5f;
    auto& transform = offscreen->registry().get<TransformComponent>(e);
    transform.position = {0.0f, height, reach};
    transform.scale = glm::vec3(3.0f, 1.0f, 3.0f);
    offscreen->updateWorldTransforms();
    FlyCameraController low;
    low.setPosition({0.0f, 4.0f, 2.0f});
    low.lookAt({0.0f, 0.0f, 0.0f});
    const CameraView lowView = low.view(1.0f);
    RenderSettings settings;
    settings.assets = &assets;
    settings.blockOnAssets = true;
    settings.virtualShadows = false;
    auto shade = [&] {
        auto image = renderer_->capture(*offscreen, lowView, {96, 96}, settings);
        EXPECT_TRUE(image.has_value());
        EXPECT_EQ(renderer_->stats().scene.culledObjects, 1u) << "the card itself is not visible";
        const glm::ivec2 shadow = toPixel(lowView, {0.0f, 0.0f, 0.0f}, {96, 96});
        const glm::ivec2 lit = toPixel(lowView, {2.5f, 0.0f, 0.0f}, {96, 96});
        return luminance(pixel(*image, shadow.x, shadow.y)) / std::max(luminance(pixel(*image, lit.x, lit.y)), 1.0f);
    };
    EXPECT_GT(shade(), 0.85f) << "a clear card casts nothing";
    mesh.baseColorTexture = "Content/Textures/solid.png";
    EXPECT_LT(shade(), 0.6f) << "a solid one does";
}
