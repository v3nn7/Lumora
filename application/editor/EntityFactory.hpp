#pragma once

#include "editor/EditorContext.hpp"

#include <QIcon>
#include <QList>

class QMenu;

namespace lumora::editor {

/// "Create" menu entries shared by the main menu, hierarchy context menu and toolbar.
void populateCreateMenu(QMenu& menu, EditorContext& ctx, bool asChild);
/// Icon representing an entity's most significant component.
[[nodiscard]] QIcon entityIcon(const Scene& scene, entt::entity e);

} // namespace lumora::editor
