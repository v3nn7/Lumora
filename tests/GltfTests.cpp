#include "GltfTestUtils.hpp"
#include "TestUtils.hpp"

#include "assets/AssetImport.hpp"
#include "assets/AssetLibrary.hpp"
#include "assets/GltfImporter.hpp"
#include "scene/ModelInstance.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <clocale>
#include <thread>
#include <string>

using namespace lumora;

TEST(Gltf, ImportsMeshMaterialAndHierarchy) {
    test::TempDir dir;
    const auto file = dir.path() / "cube.gltf";
    test::writeCubeGltf(file, false);
    auto model = importGltf(file);
    ASSERT_TRUE(model.has_value()) << model.error().message;
    ASSERT_EQ(model->meshes.size(), 1u);
    EXPECT_EQ(model->meshes[0].name, "CubeMesh");
    EXPECT_EQ(model->meshes[0].data.vertices.size(), 24u);
    EXPECT_EQ(model->meshes[0].data.indices.size(), 36u);
    EXPECT_FLOAT_EQ(model->meshes[0].data.bounds.max.x, 0.5f);
    ASSERT_EQ(model->materials.size(), 1u);
    EXPECT_FLOAT_EQ(model->materials[0].baseColor.r, 0.9f);
    EXPECT_FLOAT_EQ(model->materials[0].roughness, 0.5f);
    ASSERT_EQ(model->nodes.size(), 2u);
    EXPECT_EQ(model->nodes[0].name, "Root");
    EXPECT_EQ(model->nodes[1].parent, 0);
    EXPECT_EQ(model->nodes[1].mesh, 0);
    EXPECT_EQ(model->nodes[0].translation, glm::vec3(0, 1, 0));
    EXPECT_NEAR(glm::angle(model->nodes[1].rotation), glm::radians(90.0f), 1e-4f);
}

TEST(Gltf, RejectsBrokenFiles) {
    test::TempDir dir;
    std::ofstream(dir.path() / "bad.gltf") << "{ not json";
    EXPECT_FALSE(importGltf(dir.path() / "bad.gltf").has_value());
    EXPECT_FALSE(importGltf(dir.path() / "missing.glb").has_value());
}

TEST(Gltf, InstantiatesIntoScene) {
    test::TempDir dir;
    test::writeCubeGltf(dir.path() / "cube.gltf", false);
    auto model = importGltf(dir.path() / "cube.gltf");
    ASSERT_TRUE(model.has_value());
    Scene scene;
    const entt::entity root = instantiateModel(scene, *model, "Content/Models/cube.gltf", "Cube Model");
    EXPECT_EQ(scene.nameOf(root), "Cube Model");
    ASSERT_EQ(scene.childrenOf(root).size(), 1u);
    const entt::entity node = scene.childrenOf(root)[0];
    ASSERT_EQ(scene.childrenOf(node).size(), 1u);
    const entt::entity box = scene.childrenOf(node)[0];
    const auto& mesh = scene.registry().get<MeshRendererComponent>(box);
    EXPECT_EQ(mesh.meshAsset, "Content/Models/cube.gltf#0");
    EXPECT_FLOAT_EQ(mesh.baseColor.r, 0.9f);
    const auto& t = scene.registry().get<TransformComponent>(box);
    EXPECT_NEAR(t.rotationDeg.y, 90.0f, 1e-3f);
    EXPECT_EQ(t.scale, glm::vec3(2.0f));
    // Mesh references survive serialization.
    auto copy = SceneSerializer::clone(scene);
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ((*copy)->registry().get<MeshRendererComponent>((*copy)->findByUuid(scene.uuidOf(box))).meshAsset,
              mesh.meshAsset);
}

TEST(AssetLibrary, ResolvesAndValidatesKeys) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.path() / "Content" / "Models");
    test::writeCubeGltf(dir.path() / "Content" / "Models" / "cube.gltf", false);
    AssetLibrary library(dir.path());
    auto mesh = library.mesh("Content/Models/cube.gltf#0");
    ASSERT_TRUE(mesh.has_value()) << mesh.error().message;
    EXPECT_EQ((*mesh)->data.indices.size(), 36u);
    EXPECT_FALSE(library.mesh("Content/Models/cube.gltf#1").has_value()) << "index out of range";
    EXPECT_FALSE(library.mesh("Content/Models/cube.gltf").has_value()) << "missing index";
    EXPECT_FALSE(library.mesh("Content/Models/cube.gltf#x").has_value());
    EXPECT_FALSE(library.mesh("../outside.glb#0").has_value()) << "must stay inside the project";
    EXPECT_FALSE(library.mesh("/abs/path.glb#0").has_value());
    EXPECT_FALSE(library.mesh("Content/Models/missing.glb#0").has_value());
    auto canonical = library.canonicalKey("Content/Models/cube.gltf#0");
    ASSERT_TRUE(canonical.has_value());
    EXPECT_TRUE(canonical->starts_with(dir.path().generic_string()));
}

TEST(AssetImport, CopiesModelWithExternalBuffers) {
    test::TempDir source;
    test::TempDir project;
    test::writeCubeGltf(source.path() / "cube.gltf", true);
    const auto target = project.path() / "Content" / "Models";
    auto first = importModelFile(source.path() / "cube.gltf", target);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    EXPECT_EQ(first->filename(), "cube.gltf");
    EXPECT_TRUE(std::filesystem::exists(target / "data" / "cube.bin"));
    EXPECT_TRUE(importGltf(*first).has_value()) << "copied model must load from its new place";

    auto second = importModelFile(source.path() / "cube.gltf", target);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->filename(), "cube_1.gltf") << "never overwrite";

    std::ofstream(source.path() / "notes.txt") << "x";
    EXPECT_FALSE(importModelFile(source.path() / "notes.txt", target).has_value());
    EXPECT_TRUE(isModelFile("a/B.GLB"));
}

TEST(Gltf, ParsingIgnoresTheProcessLocale) {
    // GUI toolkits switch LC_NUMERIC to the user's locale; JSON numbers must still parse.
    const std::string previous = std::setlocale(LC_NUMERIC, nullptr);
    bool switched = false;
    for (const char* name : {"pl_PL.utf8", "pl_PL.UTF-8", "de_DE.utf8", "de_DE.UTF-8", "fr_FR.utf8"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            switched = true;
            break;
        }
    }
    if (!switched) GTEST_SKIP() << "no decimal-comma locale installed";
    test::TempDir dir;
    test::writeCubeGltf(dir.path() / "cube.gltf", false);
    auto model = importGltf(dir.path() / "cube.gltf");
    std::setlocale(LC_NUMERIC, previous.c_str());
    ASSERT_TRUE(model.has_value()) << model.error().message;
    EXPECT_FLOAT_EQ(model->materials[0].baseColor.r, 0.9f);
    EXPECT_FLOAT_EQ(model->materials[0].roughness, 0.5f);
    EXPECT_NEAR(glm::angle(model->nodes[1].rotation), glm::radians(90.0f), 1e-4f);
}

TEST(Gltf, ImportsEmbeddedTextures) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.path() / "Content" / "Models");
    test::writeCubeGltf(dir.path() / "Content" / "Models" / "textured.gltf", false, true);
    auto model = importGltf(dir.path() / "Content" / "Models" / "textured.gltf");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    ASSERT_EQ(model->images.size(), 1u);
    EXPECT_FALSE(model->images[0].encoded.empty());
    EXPECT_EQ(model->materials[0].baseColorTexture, 0);
    EXPECT_EQ(model->materials[0].normalTexture, -1);
    EXPECT_FLOAT_EQ(model->meshes[0].data.vertices[2].uv.x, 1.0f) << "TEXCOORD_0 imported";

    Scene scene;
    const entt::entity root = instantiateModel(scene, *model, "Content/Models/textured.gltf", "T");
    const entt::entity box = scene.childrenOf(scene.childrenOf(root)[0])[0];
    EXPECT_EQ(scene.registry().get<MeshRendererComponent>(box).baseColorTexture, "Content/Models/textured.gltf#tex0");

    AssetLibrary library(dir.path());
    auto texture = library.texture("Content/Models/textured.gltf#tex0");
    ASSERT_TRUE(texture.has_value()) << texture.error().message;
    EXPECT_EQ((*texture)->width, 8u);
    EXPECT_EQ((*texture)->rgba[1], 220) << "green channel";
    EXPECT_FALSE(library.texture("Content/Models/textured.gltf#tex1").has_value());
    EXPECT_FALSE(library.texture("Content/Models/textured.gltf#mesh").has_value());
}

TEST(AssetLibrary, LoadsStandaloneImageTextures) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.path() / "Content" / "Textures");
    const auto png = test::solidPng(4, 2, 10, 20, 30);
    std::ofstream(dir.path() / "Content" / "Textures" / "a.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    std::ofstream(dir.path() / "Content" / "Textures" / "broken.png") << "not a png";
    AssetLibrary library(dir.path());
    auto texture = library.texture("Content/Textures/a.png");
    ASSERT_TRUE(texture.has_value()) << texture.error().message;
    EXPECT_EQ((*texture)->width, 4u);
    EXPECT_EQ((*texture)->height, 2u);
    EXPECT_EQ((*texture)->rgba[2], 30);
    EXPECT_FALSE(library.texture("Content/Textures/broken.png").has_value());
    EXPECT_FALSE(library.texture("Content/Textures/a.txt").has_value());
    EXPECT_FALSE(library.texture("../a.png").has_value());
}

TEST(AssetLibrary, BackgroundRequestsCompleteAndShareOneLoad) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.path() / "Content" / "Models");
    test::writeCubeGltf(dir.path() / "Content" / "Models" / "textured.gltf", false, true);
    AssetLibrary library(dir.path());
    const std::string mesh = "Content/Models/textured.gltf#0";
    const std::string tex = "Content/Models/textured.gltf#tex0";

    auto first = library.requestMesh(mesh);
    EXPECT_NE(first.state, AssetState::Failed);
    auto texture = library.requestTexture(tex);
    library.waitIdle();
    first = library.requestMesh(mesh);
    texture = library.requestTexture(tex);
    ASSERT_EQ(first.state, AssetState::Ready);
    ASSERT_EQ(texture.state, AssetState::Ready);
    EXPECT_EQ(texture.value->width, 8u);

    // Blocking access returns the very same objects: each asset is loaded once.
    auto blocking = library.mesh(mesh);
    ASSERT_TRUE(blocking.has_value());
    EXPECT_EQ(*blocking, first.value);
    EXPECT_EQ(*library.texture(tex), texture.value);

    // Failures are reported as Failed, not Pending forever.
    auto missing = library.requestMesh("Content/Models/nope.glb#0");
    library.waitIdle();
    EXPECT_EQ(library.requestMesh("Content/Models/nope.glb#0").state, AssetState::Failed);
    (void)missing;
    EXPECT_EQ(library.requestTexture("../escape.png").state, AssetState::Failed);
}

TEST(AssetLibrary, ConcurrentRequestsFromManyThreads) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.path() / "Content" / "Models");
    test::writeCubeGltf(dir.path() / "Content" / "Models" / "textured.gltf", false, true);
    AssetLibrary library(dir.path());
    std::vector<std::thread> threads;
    std::vector<const ImageData*> seen(8, nullptr);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            if (t % 2 == 0) {
                auto r = library.texture("Content/Models/textured.gltf#tex0");
                if (r) seen[static_cast<std::size_t>(t)] = *r;
            } else {
                for (int i = 0; i < 1000; ++i) {
                    auto r = library.requestTexture("Content/Models/textured.gltf#tex0");
                    if (r.state == AssetState::Ready) {
                        seen[static_cast<std::size_t>(t)] = r.value;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    for (const ImageData* p : seen) EXPECT_EQ(p, seen[0]);
    EXPECT_NE(seen[0], nullptr);
}
