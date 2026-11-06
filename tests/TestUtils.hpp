#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

namespace lumora::test {

/// Unique temporary directory removed on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("lumora_test_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
        // The long spelling: Windows runners hand out 8.3 temp paths (C:/Users/RUNNER~1/...),
        // which QFileSystemModel prunes once it lists the parent folder under its real name.
        path_ = std::filesystem::canonical(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace lumora::test
