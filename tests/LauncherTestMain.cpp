#include "core/Log.hpp"

#include <QCoreApplication>

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);  // event loop for downloads
    QCoreApplication::setOrganizationName(QStringLiteral("LumoraTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LumoraLauncherTests"));
    ::testing::InitGoogleTest(&argc, argv);
    lumora::Logger::instance().setMinLevel(lumora::LogLevel::Warn);
    lumora::Logger::instance().addSink(std::make_shared<lumora::ConsoleLogSink>());
    return RUN_ALL_TESTS();
}
