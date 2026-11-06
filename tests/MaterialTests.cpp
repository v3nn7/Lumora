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
