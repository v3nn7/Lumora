#include "project/Project.hpp"

#include "core/FileSystem.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include "project/ProjectTemplates.hpp"
#include "scene/SceneSerializer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>

namespace lumora {

namespace {
constexpr std::array kTemplates{
    ProjectTemplateInfo{ProjectTemplate::Empty, "empty", "Empty", "A blank scene with a sun light and a camera."},
    ProjectTemplateInfo{ProjectTemplate::BasicScene, "basic", "Basic Scene",
                        "Ground plane, primitives and lighting - a starting point for prototyping."},
    ProjectTemplateInfo{ProjectTemplate::MaterialShowcase, "showcase", "Material Showcase",
                        "A grid of spheres sweeping metallic and roughness to inspect PBR shading."},
    ProjectTemplateInfo{ProjectTemplate::PhysicsPlayground, "physics", "Physics Playground",
                        "Rigid bodies on Jolt Physics: a tower of crates, rolling balls and a spinning paddle. Press Play."},
    ProjectTemplateInfo{ProjectTemplate::Platformer, "platformer", "Platformer",
                        "A small game: run and jump with WASD and Space, collect the coins. C++ scripts included."},
};
} // namespace

std::span<const ProjectTemplateInfo> projectTemplates() { return kTemplates; }

const ProjectTemplateInfo* findProjectTemplate(std::string_view key) {
    const auto it = std::ranges::find_if(kTemplates, [&](const auto& t) { return key == t.key; });
    return it == kTemplates.end() ? nullptr : &*it;
}

bool Project::isValidName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.') return false;
    return std::ranges::all_of(name, [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '_' || c == '-' || c == ' ';
    });
}

Result<Project> Project::create(const std::filesystem::path& directory, const std::string& name,
                                ProjectTemplate templ) {
    if (!isValidName(name)) {
        return makeError("invalid project name '{}': use letters, digits, spaces, '-' or '_' (max 64)", name);
    }
    std::error_code ec;
    if (std::filesystem::exists(directory, ec) && !std::filesystem::is_empty(directory, ec)) {
        return makeError("directory '{}' already exists and is not empty", directory.string());
    }
    std::filesystem::create_directories(directory / "Content" / "Scenes", ec);
    if (ec) return makeError("cannot create '{}': {}", directory.string(), ec.message());
    std::filesystem::create_directories(directory / "Saved", ec);

    Project project;
    project.root_ = std::filesystem::absolute(directory).lexically_normal();
    project.name_ = name;
    project.engineVersion_ = version::kString;
    project.createdUnix_ = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    project.descriptorFile_ = project.descriptorPath();

    for (const auto& [file, source] : templateScripts(templ)) {
        std::filesystem::create_directories(directory / "Scripts", ec);
        if (auto written = fs::writeTextFileAtomic(directory / "Scripts" / file, source); !written) {
            return std::unexpected(written.error());
        }
    }
    auto scene = createTemplateScene(templ);
    if (auto saved = SceneSerializer::save(*scene, project.defaultScenePath()); !saved) {
        return std::unexpected(saved.error());
    }
    if (auto saved = project.save(); !saved) {
        return std::unexpected(saved.error());
    }
    log::info("Project", "created project '{}' at {}", name, project.root_.string());
    return project;
}

Result<Project> Project::load(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path descriptor = path;
    if (std::filesystem::is_directory(path, ec)) {
        descriptor.clear();
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == kExtension) {
                descriptor = entry.path();
                break;
            }
        }
        if (descriptor.empty()) {
            return makeError("no {} descriptor found in '{}'", kExtension, path.string());
        }
    }
    auto text = fs::readTextFile(descriptor);
    if (!text) return std::unexpected(text.error());
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("format", "") != "lumora.project") {
        return makeError("'{}' is not a Lumora project descriptor", descriptor.string());
    }
    if (j.value("version", 0) > kFormatVersion) {
        return makeError("project '{}' was saved by a newer engine (format {})", descriptor.string(),
                         j.value("version", 0));
    }
    Project project;
    project.descriptorFile_ = std::filesystem::absolute(descriptor).lexically_normal();
    project.root_ = project.descriptorFile_.parent_path();
    project.name_ = j.value("name", descriptor.stem().string());
    project.description_ = j.value("description", std::string{});
    project.engineVersion_ = j.value("engineVersion", std::string{});
    project.defaultScene_ = j.value("defaultScene", project.defaultScene_);
    project.createdUnix_ = j.value("created", std::int64_t{0});
    return project;
}

Result<> Project::save() const {
    const nlohmann::json j = {
        {"format", "lumora.project"},
        {"version", kFormatVersion},
        {"name", name_},
        {"description", description_},
        {"engineVersion", version::kString},
        {"defaultScene", defaultScene_},
        {"created", createdUnix_},
    };
    // Keep the original descriptor file name even if the display name changes.
    const auto target = descriptorFile_.empty() ? descriptorPath() : descriptorFile_;
    return fs::writeTextFileAtomic(target, j.dump(2) + "\n");
}

std::string Project::relativePath(const std::filesystem::path& absolute) const {
    return std::filesystem::absolute(absolute).lexically_normal().lexically_relative(root_).generic_string();
}

} // namespace lumora
