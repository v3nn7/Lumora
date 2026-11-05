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
    auto image = renderer_->capture(*scene, cam.view(1.0f), {96, 96}, settings);
    ASSERT_TRUE(image.has_value()) << image.error().message;
    const auto center = pixel(*image, 48, 48);
    EXPECT_GT(center.g, center.r + 40) << "white material * green texture renders green";
    EXPECT_GT(center.g, center.b + 40);
}
