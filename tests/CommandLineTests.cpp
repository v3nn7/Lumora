#include "main/CommandLine.hpp"

#include <gtest/gtest.h>

using namespace lumora::app;

TEST(CommandLine, NoArgumentsOpensHub) {
    auto o = parseCommandLine({});
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->mode, AppMode::Hub);
}

TEST(CommandLine, EditorMode) {
    auto o = parseCommandLine({"--editor", "/tmp/Game"});
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->mode, AppMode::Editor);
    EXPECT_EQ(o->projectPath, "/tmp/Game");
}

TEST(CommandLine, PlayModeRequiresProject) {
    EXPECT_FALSE(parseCommandLine({"--play"}).has_value());
    auto o = parseCommandLine({"--project", "Game", "--play"});
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->mode, AppMode::Play);
}

TEST(CommandLine, HeadlessDefaultsToInfo) {
    auto o = parseCommandLine({"--headless"});
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->mode, AppMode::Headless);
    EXPECT_TRUE(o->printInfo);
}

TEST(CommandLine, HeadlessRender) {
    auto o = parseCommandLine({"--headless", "--project", "P", "--render", "out.png", "--width", "640", "--height", "360"});
    ASSERT_TRUE(o.has_value()) << o.error().message;
    EXPECT_EQ(o->renderOutput, "out.png");
    EXPECT_EQ(o->renderWidth, 640u);
    EXPECT_EQ(o->renderHeight, 360u);
    EXPECT_FALSE(o->printInfo);
    EXPECT_FALSE(parseCommandLine({"--headless", "--render", "out.png"}).has_value()) << "needs --project";
}

TEST(CommandLine, RejectsBadInput) {
    EXPECT_FALSE(parseCommandLine({"--bogus"}).has_value());
    EXPECT_FALSE(parseCommandLine({"--editor"}).has_value());
    EXPECT_FALSE(parseCommandLine({"--gpu", "abc"}).has_value());
    EXPECT_FALSE(parseCommandLine({"--editor", "A", "--headless"}).has_value());
    EXPECT_FALSE(parseCommandLine({"--log-level", "loud"}).has_value());
    EXPECT_FALSE(parseCommandLine({"--headless", "--project", "P", "--render", "x.png", "--width", "0"}).has_value());
}

TEST(CommandLine, GlobalOptions) {
    auto o = parseCommandLine({"--gpu", "1", "--no-validation", "--no-vsync", "--log-level", "debug"});
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->gpuIndex, 1u);
    EXPECT_EQ(o->validation, false);
    EXPECT_EQ(o->vsync, false);
    EXPECT_EQ(o->logLevel, lumora::LogLevel::Debug);
}

TEST(CommandLine, JoinAndHostSessions) {
    auto join = parseCommandLine({"--join", "bright-fox.at.ply.gg:23145", "--user", "Ada", "--password", "pw"});
    ASSERT_TRUE(join.has_value());
    EXPECT_EQ(join->mode, AppMode::Join);
    EXPECT_EQ(join->joinAddress, "bright-fox.at.ply.gg:23145");
    EXPECT_EQ(join->userName, "Ada");
    EXPECT_EQ(join->sessionPassword, "pw");

    auto host = parseCommandLine({"--editor", "Game", "--host", "7777"});
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(host->mode, AppMode::Editor);
    EXPECT_EQ(host->hostPort, std::optional<std::uint16_t>(7777));

    EXPECT_FALSE(parseCommandLine({"--host", "7777"}).has_value());                       // no project
    EXPECT_FALSE(parseCommandLine({"--editor", "Game", "--host", "0"}).has_value());      // bad port
    EXPECT_FALSE(parseCommandLine({"--editor", "Game", "--host", "70000"}).has_value());
    EXPECT_FALSE(parseCommandLine({"--join", "x:1", "--editor", "Game"}).has_value());    // exclusive
    EXPECT_FALSE(parseCommandLine({"--join"}).has_value());
}
