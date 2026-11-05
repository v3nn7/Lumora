#include "TestUtils.hpp"

#include "core/FileSystem.hpp"
#include "core/Log.hpp"
#include "core/Uuid.hpp"

#include <set>

using namespace lumora;

TEST(Uuid, GeneratesUniqueNonZeroValues) {
    std::set<std::uint64_t> seen;
    for (int i = 0; i < 10000; ++i) {
        const Uuid id = Uuid::generate();
        EXPECT_TRUE(id.isValid());
        EXPECT_TRUE(seen.insert(id.value()).second);
    }
}

TEST(Uuid, StringRoundTrip) {
    const Uuid id(0x0123456789abcdefULL);
    EXPECT_EQ(id.toString(), "0123456789abcdef");
    auto parsed = Uuid::parse(id.toString());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, id);
}

TEST(Uuid, RejectsMalformedText) {
    EXPECT_FALSE(Uuid::parse("").has_value());
    EXPECT_FALSE(Uuid::parse("xyz").has_value());
    EXPECT_FALSE(Uuid::parse("0123456789abcdef0").has_value());
    EXPECT_FALSE(Uuid::parse("12 34").has_value());
}

TEST(FileSystem, AtomicWriteAndRead) {
    test::TempDir dir;
    const auto file = dir.path() / "nested" / "file.txt";
    ASSERT_TRUE(fs::writeTextFileAtomic(file, "hello lumora").has_value());
    auto text = fs::readTextFile(file);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "hello lumora");
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp"));
    ASSERT_TRUE(fs::writeTextFileAtomic(file, "replaced").has_value());
    EXPECT_EQ(*fs::readTextFile(file), "replaced");
}

TEST(FileSystem, MissingFileIsAnError) {
    auto result = fs::readTextFile("/definitely/not/here.txt");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("cannot open"), std::string::npos);
}

namespace {
struct CaptureSink final : LogSink {
    std::vector<LogMessage> messages;
    void write(const LogMessage& m) override { messages.push_back(m); }
};
} // namespace

TEST(Logger, DispatchesToSinksRespectingLevel) {
    auto sink = std::make_shared<CaptureSink>();
    Logger& logger = Logger::instance();
    const LogLevel previous = logger.minLevel();
    logger.addSink(sink);
    logger.setMinLevel(LogLevel::Info);
    log::debug("Test", "hidden {}", 1);
    log::info("Test", "visible {}", 2);
    log::error("Test", "error {}", 3);
    logger.removeSink(sink);
    logger.setMinLevel(previous);
    ASSERT_EQ(sink->messages.size(), 2u);
    EXPECT_EQ(sink->messages[0].text, "visible 2");
    EXPECT_EQ(sink->messages[0].category, "Test");
    EXPECT_EQ(sink->messages[1].level, LogLevel::Error);
    EXPECT_NE(formatLogLine(sink->messages[1]).find("[error] [Test] error 3"), std::string::npos);
}
