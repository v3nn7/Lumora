// GPU integration tests: render real frames offscreen and inspect the pixels.
// Skipped (not faked) when no Vulkan 1.4 device is available.
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>

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
        if (!renderer_) GTEST_SKIP() << "no suitable Vulkan 1.4 GPU: " << skipReason_;
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
    auto image = renderer_->capture(*scene, view, {size.x, size.y}, settings);
    ASSERT_TRUE(image.has_value());
    const glm::u8vec4 near = pixel(*image, toPixel(view, {0.0f, 0.0f, -3.0f}, size).x, toPixel(view, {0.0f, 0.0f, -3.0f}, size).y);
    const glm::ivec2 farPx = toPixel(view, {0.0f, 0.0f, -40.0f}, size);
    const glm::u8vec4 far = pixel(*image, farPx.x, farPx.y);
    EXPECT_GT(near.r, near.g + 40) << "cascade 0 is tinted red";
    EXPECT_GT(far.g, far.b + 40) << "cascade 3 is tinted yellow";
    EXPECT_GT(far.r, far.b + 40);
}
