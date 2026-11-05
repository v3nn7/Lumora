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
    EXPECT_FALSE(o->vsync);
    EXPECT_EQ(o->logLevel, lumora::LogLevel::Debug);
}
