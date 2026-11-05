#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace lumora {

enum class ProjectTemplate { Empty, BasicScene, MaterialShowcase };

struct ProjectTemplateInfo {
    ProjectTemplate id;
    const char* key;          // CLI / serialization key
    const char* displayName;
    const char* description;
};

[[nodiscard]] std::span<const ProjectTemplateInfo> projectTemplates();
[[nodiscard]] const ProjectTemplateInfo* findProjectTemplate(std::string_view key);

/// A project is a directory containing <Name>.lumora (JSON descriptor) plus Content/ and Saved/.
///
///   MyGame/
///     MyGame.lumora
///     Content/Scenes/Main.lscene
///     Saved/Thumbnail.png, Saved/Logs/
class Project {
public:
    static constexpr const char* kExtension = ".lumora";
    static constexpr int kFormatVersion = 1;

    /// Creates the directory layout and a default scene from the template. Fails if the
    /// target directory exists and is not empty.
    [[nodiscard]] static Result<Project> create(const std::filesystem::path& directory, const std::string& name,
                                                ProjectTemplate templ);
    /// Accepts either the .lumora descriptor or the project directory.
    [[nodiscard]] static Result<Project> load(const std::filesystem::path& path);
    [[nodiscard]] static bool isValidName(std::string_view name);

    [[nodiscard]] Result<> save() const;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }
    void setDescription(std::string d) { description_ = std::move(d); }
    [[nodiscard]] const std::string& engineVersion() const noexcept { return engineVersion_; }
    [[nodiscard]] std::int64_t createdUnix() const noexcept { return createdUnix_; }

    /// Default scene, relative to the project root using '/' separators.
    [[nodiscard]] const std::string& defaultScene() const noexcept { return defaultScene_; }
    void setDefaultScene(std::string relative) { defaultScene_ = std::move(relative); }

    [[nodiscard]] const std::filesystem::path& rootDir() const noexcept { return root_; }
    [[nodiscard]] std::filesystem::path descriptorPath() const { return root_ / (name_ + kExtension); }
    [[nodiscard]] std::filesystem::path contentDir() const { return root_ / "Content"; }
    [[nodiscard]] std::filesystem::path scenesDir() const { return contentDir() / "Scenes"; }
    [[nodiscard]] std::filesystem::path savedDir() const { return root_ / "Saved"; }
    [[nodiscard]] std::filesystem::path thumbnailPath() const { return savedDir() / "Thumbnail.png"; }
    [[nodiscard]] std::filesystem::path defaultScenePath() const { return root_ / defaultScene_; }
    /// Converts an absolute path under the project into a portable relative string.
    [[nodiscard]] std::string relativePath(const std::filesystem::path& absolute) const;

private:
    std::filesystem::path root_;
    std::filesystem::path descriptorFile_;
    std::string name_;
    std::string description_;
    std::string engineVersion_;
    std::string defaultScene_ = "Content/Scenes/Main.lscene";
    std::int64_t createdUnix_ = 0;
};

} // namespace lumora
