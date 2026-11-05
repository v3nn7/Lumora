#include "core/Log.hpp"

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    // Keep test output readable: only warnings and errors from the engine.
    lumora::Logger::instance().setMinLevel(lumora::LogLevel::Warn);
    lumora::Logger::instance().addSink(std::make_shared<lumora::ConsoleLogSink>());
    return RUN_ALL_TESTS();
}
