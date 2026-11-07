#include "TestUtils.hpp"

#include "assets/AssetLibrary.hpp"
#include "assets/Material.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <thread>

using namespace lumora;

TEST(Material, SavesAndLoadsEveryField) {
    test::TempDir dir;
    MaterialAsset m;
    m.baseColor = {0.9f, 0.1f, 0.2f};
    m.metallic = 1.0f;
    m.roughness = 0.25f;
    m.emissive = {0.0f, 1.0f, 0.5f};
    m.emissiveStrength = 4.0f;
    m.pattern = 2;
    m.baseColorTexture = "Content/Textures/a.png";
    m.metallicRoughnessTexture = "Content/Models/x.glb#tex1";
    m.normalTexture = "Content/Textures/n.png";
    m.emissiveTexture = "Content/Textures/e.png";
    m.occlusionTexture = "Content/Textures/ao.png";
    m.normalStrength = 0.5f;
    m.alphaMode = MaterialAsset::AlphaMode::CutOut;
    m.alphaCutoff = 0.3f;
    m.opacity = 0.75f;
    m.doubleSided = true;
    m.uvScale = {2.0f, 3.0f};
    m.uvOffset = {0.25f, -0.5f};
    m.uvRotation = 45.0f;
    m.ior = 1.33f;
    m.specularFactor = 0.5f;
    m.specularColor = {1.0f, 0.5f, 0.25f};
    m.clearcoat = 0.8f;
    m.clearcoatRoughness = 0.1f;
    m.sheenColor = {0.2f, 0.3f, 0.4f};
    m.sheenRoughness = 0.6f;
    m.transmission = 0.9f;
    m.thickness = 0.05f;
    m.attenuationDistance = 0.2f;
    m.attenuationColor = {0.9f, 0.5f, 0.1f};
    const auto file = dir.path() / "Content" / "Materials" / "Red.lmat";
    ASSERT_TRUE(saveMaterial(m, file).has_value());
    auto loaded = loadMaterial(file);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(*loaded, m);
}

TEST(Material, MissingFieldsKeepDefaultsAndBadOnesAreErrors) {
    auto partial = materialFromJson(nlohmann::json{{"roughness", 0.9}});
    ASSERT_TRUE(partial.has_value());
    EXPECT_FLOAT_EQ(partial->roughness, 0.9f);
    EXPECT_EQ(partial->baseColor, MaterialAsset{}.baseColor);
    EXPECT_FALSE(materialFromJson(nlohmann::json{{"baseColor", "red"}}).has_value());
    EXPECT_FALSE(materialFromJson(nlohmann::json{{"metallic", "1"}}).has_value());
    EXPECT_FALSE(materialFromJson(nlohmann::json::array()).has_value());
    EXPECT_FALSE(materialFromJson(nlohmann::json{{"alphaMode", 3}}).has_value());
    EXPECT_FALSE(materialFromJson(nlohmann::json{{"doubleSided", 1}}).has_value());
    EXPECT_FALSE(materialFromJson(nlohmann::json{{"uvScale", {1, 2, 3}}}).has_value());
    EXPECT_EQ(partial->alphaMode, MaterialAsset::AlphaMode::Opaque) << "files from before keep their look";
    EXPECT_FLOAT_EQ(partial->ior, 1.5f);
    // Tiling, offset and rotation (degrees) make the texture transform.
    MaterialAsset tiled;
    tiled.uvScale = {2.0f, 4.0f};
    tiled.uvOffset = {0.5f, 0.25f};
    tiled.uvRotation = 90.0f;
    const TextureTransform t = tiled.uvTransform();
    EXPECT_NEAR(t.rotation, 1.5707963f, 1e-6f);
    EXPECT_EQ(t.scale, glm::vec2(2.0f, 4.0f));
    EXPECT_EQ(t.offset, glm::vec2(0.5f, 0.25f));
    test::TempDir dir;
    std::ofstream(dir.path() / "broken.lmat") << "{ not json";
    EXPECT_FALSE(loadMaterial(dir.path() / "broken.lmat").has_value());
}

TEST(Material, LibrarySharesOneCopyAndPicksUpChanges) {
    test::TempDir dir;
    AssetLibrary assets(dir.path());
    MaterialAsset m;
    m.baseColor = {1.0f, 0.0f, 0.0f};
    ASSERT_TRUE(saveMaterial(m, dir.path() / "Content" / "M.lmat").has_value());
    auto first = assets.material("Content/M.lmat");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(assets.material("Content/M.lmat"), first) << "cached";
    EXPECT_EQ(assets.material("Content/Missing.lmat"), nullptr);
    EXPECT_EQ(assets.material("../outside.lmat"), nullptr);

    m.baseColor = {0.0f, 0.0f, 1.0f};
    ASSERT_TRUE(saveMaterial(m, dir.path() / "Content" / "M.lmat").has_value());
    assets.reloadMaterial("Content/M.lmat");  // what the editor does after writing it
    auto second = assets.material("Content/M.lmat");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->baseColor, glm::vec3(0.0f, 0.0f, 1.0f));
    EXPECT_EQ(first->baseColor, glm::vec3(1.0f, 0.0f, 0.0f)) << "holders of the old copy keep it";

    // Changed by something else (a text editor, git): seen within half a second.
    m.roughness = 0.75f;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // a distinct modification time
    ASSERT_TRUE(saveMaterial(m, dir.path() / "Content" / "M.lmat").has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    auto third = assets.material("Content/M.lmat");
    ASSERT_NE(third, nullptr);
    EXPECT_FLOAT_EQ(third->roughness, 0.75f);
}
