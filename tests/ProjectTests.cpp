#include "TestUtils.hpp"

#include <fstream>

#include "project/Project.hpp"
#include "project/ProjectRegistry.hpp"
#include "project/ProjectTemplates.hpp"
#include "scene/SceneSerializer.hpp"

using namespace lumora;

TEST(Project, NameValidation) {
    EXPECT_TRUE(Project::isValidName("My Game"));
    EXPECT_TRUE(Project::isValidName("game_01-x"));
    EXPECT_FALSE(Project::isValidName(""));
    EXPECT_FALSE(Project::isValidName(" lead"));
    EXPECT_FALSE(Project::isValidName("bad/slash"));
    EXPECT_FALSE(Project::isValidName(".hidden"));
    EXPECT_FALSE(Project::isValidName(std::string(65, 'a')));
}

TEST(Project, CreateLoadSaveRoundTrip) {
    test::TempDir dir;
    const auto root = dir.path() / "Demo";
    auto created = Project::create(root, "Demo", ProjectTemplate::BasicScene);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    EXPECT_TRUE(std::filesystem::exists(root / "Demo.lumora"));
    EXPECT_TRUE(std::filesystem::exists(created->defaultScenePath()));

    auto byDir = Project::load(root);
    ASSERT_TRUE(byDir.has_value()) << byDir.error().message;
    EXPECT_EQ(byDir->name(), "Demo");
    auto byFile = Project::load(root / "Demo.lumora");
    ASSERT_TRUE(byFile.has_value());
    EXPECT_EQ(byFile->rootDir(), byDir->rootDir());

    byFile->setDescription("A test project");
    ASSERT_TRUE(byFile->save().has_value());
    auto reloaded = Project::load(root);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->description(), "A test project");

    auto scene = SceneSerializer::load(reloaded->defaultScenePath());
    ASSERT_TRUE(scene.has_value()) << scene.error().message;
    EXPECT_GT((*scene)->entityCount(), 3u);
    EXPECT_NE((*scene)->primaryCamera(), entt::entity{entt::null});
}

TEST(Project, CreateRefusesNonEmptyDirectory) {
    test::TempDir dir;
    std::ofstream(dir.path() / "existing.txt") << "user data";
    auto created = Project::create(dir.path(), "Demo", ProjectTemplate::Empty);
    ASSERT_FALSE(created.has_value());
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "existing.txt")) << "must never touch existing files";
}

TEST(Project, LoadRejectsGarbage) {
    test::TempDir dir;
    EXPECT_FALSE(Project::load(dir.path()).has_value()) << "no descriptor";
    std::ofstream(dir.path() / "X.lumora") << "{ not json";
    EXPECT_FALSE(Project::load(dir.path() / "X.lumora").has_value());
}

TEST(ProjectTemplates, AllTemplatesProduceSerializableScenes) {
    for (const ProjectTemplateInfo& info : projectTemplates()) {
        auto scene = createTemplateScene(info.id);
        ASSERT_NE(scene, nullptr);
        EXPECT_NE(scene->primaryCamera(), entt::entity{entt::null}) << info.key;
        auto roundTrip = SceneSerializer::clone(*scene);
        ASSERT_TRUE(roundTrip.has_value()) << info.key;
        EXPECT_EQ((*roundTrip)->entityCount(), scene->entityCount());
        EXPECT_EQ(findProjectTemplate(info.key), &info);
    }
    EXPECT_EQ(findProjectTemplate("nope"), nullptr);
}

TEST(ProjectRegistry, TouchOrdersByRecencyAndPersists) {
    test::TempDir dir;
    const auto storage = dir.path() / "recent.json";
    {
        ProjectRegistry reg(storage);
        ASSERT_TRUE(reg.load().has_value()) << "missing file is an empty list";
        reg.touch(dir.path() / "A" / "A.lumora", "A");
        reg.touch(dir.path() / "B" / "B.lumora", "B");
        reg.touch(dir.path() / "A" / "A.lumora", "A renamed");
        ASSERT_EQ(reg.entries().size(), 2u);
        EXPECT_EQ(reg.entries()[0].name, "A renamed");
        EXPECT_EQ(reg.entries()[1].name, "B");
        ASSERT_TRUE(reg.save().has_value());
    }
    ProjectRegistry reg(storage);
    ASSERT_TRUE(reg.load().has_value());
    ASSERT_EQ(reg.entries().size(), 2u);
    EXPECT_EQ(reg.entries()[0].name, "A renamed");
    EXPECT_TRUE(reg.remove(dir.path() / "B" / "B.lumora"));
    EXPECT_FALSE(reg.remove(dir.path() / "B" / "B.lumora"));
    EXPECT_EQ(reg.entries().size(), 1u);
}

TEST(ProjectRegistry, CorruptFileReportsError) {
    test::TempDir dir;
    std::ofstream(dir.path() / "recent.json") << "garbage";
    ProjectRegistry reg(dir.path() / "recent.json");
    EXPECT_FALSE(reg.load().has_value());
}
