#include "core/Log.hpp"

#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <cstdlib>

int main(int argc, char** argv) {
    // Never touch the user's real configuration (recent projects, editor layout).
    QTemporaryDir configDir;
    qputenv("XDG_CONFIG_HOME", configDir.path().toUtf8());
    qputenv("XDG_STATE_HOME", configDir.path().toUtf8());
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setOrganizationName(QStringLiteral("LumoraTests"));
    QApplication::setApplicationName(QStringLiteral("LumoraUiTests"));
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    lumora::Logger::instance().setMinLevel(lumora::LogLevel::Warn);
    return RUN_ALL_TESTS();
}
