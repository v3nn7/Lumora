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
#include <functional>
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

// ---- Animations -----------------------------------------------------------------------------

#include "runtime/AnimationSystem.hpp"
#include "runtime/Simulation.hpp"

TEST(GltfAnimation, ImportsAndSamplesClips) {
    test::TempDir dir;
    test::writeAnimatedGltf(dir.path() / "arm.gltf");
    auto model = importGltf(dir.path() / "arm.gltf");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    ASSERT_EQ(model->animations.size(), 2u);
    const ImportedAnimation& slide = model->animations[0];
    EXPECT_EQ(slide.name, "Slide");
    EXPECT_FLOAT_EQ(slide.duration, 1.0f);
    ASSERT_EQ(slide.channels.size(), 2u);
    EXPECT_FLOAT_EQ(model->animations[1].duration, 2.0f);

    std::vector<NodePose> poses(model->nodes.size());
    sampleAnimation(slide, 0.25f, poses);
    EXPECT_NEAR(poses[0].translation.x, 0.5f, 1e-5f) << "linear: a quarter of the way to x = 2";
    EXPECT_NEAR(poses[1].rotation.w, 1.0f, 1e-5f) << "step: still the first key before 0.5 s";
    sampleAnimation(slide, 0.75f, poses);
    EXPECT_NEAR(poses[0].translation.x, 1.5f, 1e-5f);
    EXPECT_NEAR(glm::degrees(glm::angle(poses[1].rotation)), 90.0f, 0.01f) << "step: jumped to the second key";
    sampleAnimation(slide, 5.0f, poses);
    EXPECT_NEAR(poses[0].translation.x, 2.0f, 1e-5f) << "clamped past the end";
    EXPECT_NEAR(poses[1].translation.y, 0.0f, 1e-6f) << "untouched: no channel animates Hand's translation";
}

TEST(GltfAnimation, AnimatorDrivesTheModelsEntities) {
    test::TempDir dir;
    test::writeAnimatedGltf(dir.path() / "Content" / "Models" / "arm.gltf");
    AssetLibrary assets(dir.path());
    auto model = assets.model(dir.path() / "Content" / "Models" / "arm.gltf");
    ASSERT_TRUE(model.has_value());
    Scene scene("Anim");
    const entt::entity root = instantiateModel(scene, **model, "Content/Models/arm.gltf", "Robot");
    ASSERT_TRUE(scene.registry().all_of<AnimatorComponent>(root)) << "models with clips get an Animator";
    entt::entity arm = entt::null, hand = entt::null;
    for (auto [e, n] : scene.registry().view<NameComponent>().each()) {
        if (n.name == "Arm") arm = e;
        if (n.name == "Hand") hand = e;
    }
    ASSERT_TRUE(arm != entt::null && hand != entt::null);

    Simulation sim(scene);
    sim.setProjectRoot(dir.path());
    ASSERT_NE(sim.animation(), nullptr);
    for (int i = 0; i < 30; ++i) sim.step();  // 0.5 s into "Slide"
    EXPECT_NEAR(scene.registry().get<TransformComponent>(arm).position.x, 1.0f, 0.02f);
    EXPECT_NEAR(scene.registry().get<TransformComponent>(hand).position.y, 1.0f, 1e-5f) << "not animated: kept";
    EXPECT_NEAR(std::abs(scene.registry().get<TransformComponent>(hand).rotationDeg.y), 90.0f, 0.1f);
    for (int i = 0; i < 45; ++i) sim.step();  // 1.25 s: looped to 0.25 s
    EXPECT_NEAR(scene.registry().get<TransformComponent>(arm).position.x, 0.5f, 0.02f) << "loops by default";

    // Switch clips and play once.
    auto& animator = scene.registry().get<AnimatorComponent>(root);
    animator.loop = false;
    ASSERT_TRUE(sim.animation()->play(root, "Lift"));
    for (int i = 0; i < 180; ++i) sim.step();  // 3 s > 2 s clip
    EXPECT_NEAR(scene.registry().get<TransformComponent>(arm).position.y, 3.0f, 1e-4f) << "stops on the last pose";
    EXPECT_FALSE(sim.animation()->isPlaying(root));
}

TEST(GltfMaterials, SplitsAMeshPerMaterial) {
    // One glTF mesh, two primitives with different materials (a red and a blue triangle).
    test::TempDir dir;
    std::vector<unsigned char> bin;
    const float positions[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {3, 0, 0}, {2, 1, 0}};
    const auto* p = reinterpret_cast<const unsigned char*>(positions);
    bin.insert(bin.end(), p, p + sizeof positions);
    const std::string json = std::format(R"({{
  "asset": {{"version": "2.0"}}, "scene": 0, "scenes": [{{"nodes": [0]}}],
  "nodes": [{{"name": "Sign", "mesh": 0}}],
  "meshes": [{{"name": "Sign", "primitives": [{{"attributes": {{"POSITION": 0}}, "material": 0}},
                                              {{"attributes": {{"POSITION": 1}}, "material": 1}}]}}],
  "materials": [{{"name": "Red", "pbrMetallicRoughness": {{"baseColorFactor": [1, 0, 0, 1]}}}},
                {{"name": "Blue", "pbrMetallicRoughness": {{"baseColorFactor": [0, 0, 1, 1]}}}}],
  "buffers": [{{"byteLength": {}, "uri": "data:application/octet-stream;base64,{}"}}],
  "bufferViews": [{{"buffer": 0, "byteOffset": 0, "byteLength": 36}}, {{"buffer": 0, "byteOffset": 36, "byteLength": 36}}],
  "accessors": [{{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]}},
                {{"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3", "min": [2,0,0], "max": [3,1,0]}}]
}})", bin.size(), test::base64(bin));
    std::ofstream(dir.path() / "sign.gltf") << json;
    auto model = importGltf(dir.path() / "sign.gltf");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    ASSERT_EQ(model->meshes.size(), 2u) << "one part per material";
    ASSERT_EQ(model->meshParts.size(), 1u);
    EXPECT_EQ(model->meshParts[0], (std::vector<int>{0, 1}));
    EXPECT_EQ(model->meshes[0].material, 0);
    EXPECT_EQ(model->meshes[1].material, 1);
    EXPECT_EQ(model->meshes[0].data.vertices.size(), 3u);
    EXPECT_FLOAT_EQ(model->meshes[1].data.bounds.min.x, 2.0f);

    Scene scene("Materials");
    const entt::entity root = instantiateModel(scene, *model, "sign.gltf", "Sign");
    const entt::entity node = scene.childrenOf(root).at(0);
    const auto& first = scene.registry().get<MeshRendererComponent>(node);
    EXPECT_EQ(first.meshAsset, "sign.gltf#0");
    EXPECT_EQ(first.baseColor, glm::vec3(1, 0, 0));
    ASSERT_EQ(scene.childrenOf(node).size(), 1u) << "the second material is a child part";
    const auto& second = scene.registry().get<MeshRendererComponent>(scene.childrenOf(node)[0]);
    EXPECT_EQ(second.meshAsset, "sign.gltf#1");
    EXPECT_EQ(second.baseColor, glm::vec3(0, 0, 1));
}

TEST(GltfAnimation, CrossfadeBlendsFromTheOldClipToTheNew) {
    test::TempDir dir;
    test::writeAnimatedGltf(dir.path() / "Content" / "Models" / "arm.gltf");
    AssetLibrary assets(dir.path());
    auto model = assets.model(dir.path() / "Content" / "Models" / "arm.gltf");
    ASSERT_TRUE(model.has_value());
    Scene scene("Blend");
    const entt::entity root = instantiateModel(scene, **model, "Content/Models/arm.gltf", "Robot");
    entt::entity arm = entt::null;
    for (auto [e, n] : scene.registry().view<NameComponent>().each()) {
        if (n.name == "Arm") arm = e;
    }
    Simulation sim(scene, 1.0 / 60.0);
    sim.setProjectRoot(dir.path());
    for (int i = 0; i < 15; ++i) sim.step();  // Slide at 0.25 s: x = 0.5
    const auto& t = scene.registry().get<TransformComponent>(arm);
    EXPECT_NEAR(t.position.x, 0.5f, 0.02f);

    ASSERT_TRUE(sim.animation()->crossfade(root, "Lift", 1.0f));
    EXPECT_NEAR(sim.animation()->blendWeight(root), 0.0f, 1e-6f);
    EXPECT_NEAR(t.position.x, 0.5f, 0.02f) << "no pop when the fade starts";
    for (int i = 0; i < 30; ++i) sim.step();  // half way: Slide at 0.75 (x 1.5), Lift at 0.5 (y 0.75)
    EXPECT_NEAR(sim.animation()->blendWeight(root), 0.5f, 0.02f);
    EXPECT_NEAR(t.position.x, 0.75f, 0.05f) << "between the clips";
    EXPECT_NEAR(t.position.y, 0.375f, 0.03f);
    for (int i = 0; i < 36; ++i) sim.step();  // fade over; Lift at 1.1 s
    EXPECT_FLOAT_EQ(sim.animation()->blendWeight(root), 1.0f);
    EXPECT_NEAR(t.position.x, 0.0f, 1e-4f) << "the new clip alone";
    EXPECT_NEAR(t.position.y, 1.65f, 0.03f);
}

#include <nlohmann/json.hpp>

#include <fstream>

namespace {
/// The textured test cube with its JSON edited.
std::filesystem::path editedCube(const std::filesystem::path& dir, const std::string& name,
                                 const std::function<void(nlohmann::json&)>& edit) {
    const auto file = dir / name;
    test::writeCubeGltf(file, false, true);
    nlohmann::json doc;
    std::ifstream(file) >> doc;
    edit(doc);
    std::ofstream(file) << doc.dump();
    return file;
}
} // namespace

TEST(GltfMaterials, CutOutTwoSidedAndTheModelsOwnUnits) {
    test::TempDir dir;
    const auto masked = editedCube(dir.path(), "masked.gltf", [](nlohmann::json& doc) {
        doc["materials"][0]["alphaMode"] = "MASK";
        doc["materials"][0]["alphaCutoff"] = 0.3;
        doc["materials"][0]["doubleSided"] = true;
    });
    auto model = importGltf(masked);
    ASSERT_TRUE(model.has_value()) << model.error().message;
    EXPECT_EQ(model->materials[0].alphaMode, ImportedMaterial::AlphaMode::Mask);
    EXPECT_FLOAT_EQ(model->materials[0].alphaCutoff, 0.3f);
    EXPECT_TRUE(model->materials[0].doubleSided);
    EXPECT_TRUE(model->materials[0].baseColorHasAlpha) << "the test image is an RGBA PNG";
    EXPECT_FLOAT_EQ(model->unitScale, 1.0f) << "glTF is in metres";
    EXPECT_GT(model->boundsMax.x - model->boundsMin.x, 0.5f);
    Scene scene;
    const entt::entity root = instantiateModel(scene, *model, "Content/Models/masked.gltf", "M");
    const entt::entity box = scene.childrenOf(scene.childrenOf(root)[0])[0];
    const auto& mesh = scene.registry().get<MeshRendererComponent>(box);
    EXPECT_FLOAT_EQ(mesh.alphaCutoff, 0.3f);
    EXPECT_TRUE(mesh.doubleSided);
    EXPECT_EQ(scene.registry().get<TransformComponent>(root).scale, glm::vec3(1.0f));
    // Both survive saving.
    auto copy = SceneSerializer::clone(scene);
    ASSERT_TRUE(copy.has_value());
    const auto& copied = (*copy)->registry().get<MeshRendererComponent>((*copy)->findByUuid(scene.uuidOf(box)));
    EXPECT_FLOAT_EQ(copied.alphaCutoff, 0.3f);
    EXPECT_TRUE(copied.doubleSided);

    // BLEND (leaf cards) is cut out at a half.
    const auto blended = editedCube(dir.path(), "blend.gltf", [](nlohmann::json& doc) { doc["materials"][0]["alphaMode"] = "BLEND"; });
    auto blend = importGltf(blended);
    ASSERT_TRUE(blend.has_value());
    Scene blendScene;
    const entt::entity blendRoot = instantiateModel(blendScene, *blend, "Content/Models/blend.gltf", "B");
    EXPECT_FLOAT_EQ(blendScene.registry().get<MeshRendererComponent>(blendScene.childrenOf(blendScene.childrenOf(blendRoot)[0])[0]).alphaCutoff,
                    0.5f);

    // A SketchUp model (inches written as metres) comes in at its real size, both sides drawn.
    const auto sketchUp = editedCube(dir.path(), "sketchup.gltf", [](nlohmann::json& doc) { doc["nodes"][0]["name"] = "SketchUp"; });
    auto inches = importGltf(sketchUp);
    ASSERT_TRUE(inches.has_value());
    EXPECT_FLOAT_EQ(inches->unitScale, 0.0254f);
    EXPECT_TRUE(inches->materials[0].doubleSided);
    Scene inchScene;
    const entt::entity inchRoot = instantiateModel(inchScene, *inches, "Content/Models/sketchup.gltf", "S");
    EXPECT_EQ(inchScene.registry().get<TransformComponent>(inchRoot).scale, glm::vec3(0.0254f));
}
