#pragma once

#include "project/Project.hpp"
#include "scene/Scene.hpp"

#include <memory>

namespace lumora {

[[nodiscard]] std::unique_ptr<Scene> createTemplateScene(ProjectTemplate templ);

} // namespace lumora
