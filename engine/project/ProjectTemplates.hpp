#pragma once

#include "project/Project.hpp"
#include "scene/Scene.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lumora {

[[nodiscard]] std::unique_ptr<Scene> createTemplateScene(ProjectTemplate templ);
/// Gameplay scripts a template comes with: (file name in Scripts/, source).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> templateScripts(ProjectTemplate templ);

} // namespace lumora
