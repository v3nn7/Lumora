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
    auto has = [&](std::uint32_t x, std::uint32_t y, std::uint32_t light) {
        const std::uint32_t t = y * grid.tilesX + x;
        for (std::uint32_t k = 0; k < grid.data[t * 2 + 1]; ++k) {
            if (grid.data[grid.data[t * 2] + k] == light) return true;
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

// ---- HUD ------------------------------------------------------------------------------------

#include "renderer/HudRenderer.hpp"

TEST(HudFont, MeasuresTextFromTheEmbeddedFont) {
    const HudFont font;
    EXPECT_GT(font.measure("Hello", 24.0f), 40.0f);
    EXPECT_NEAR(font.measure("Hello", 48.0f), 2.0f * font.measure("Hello", 24.0f), 0.01f) << "scales with size";
    EXPECT_GT(font.measure("WWW", 24.0f), font.measure("iii", 24.0f));
    EXPECT_GT(font.measure("Zażółć gęślą jaźń", 24.0f), 0.0f) << "Polish letters are in the font";
    EXPECT_NEAR(font.measure("ab\nabcd", 24.0f), font.measure("abcd", 24.0f), 0.01f) << "widest line";
    std::vector<HudVertex> vertices;
    HudCanvas canvas;
    canvas.addRect({0, 0}, {10, 10}, {1, 1, 1, 1});
    canvas.addText({0, 0}, 20.0f, {1, 1, 1, 1}, "A b");
    font.layout(canvas, vertices);
    EXPECT_EQ(vertices.size(), 6u * 3u) << "one quad for the rectangle, one per visible glyph (A, b)";
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
