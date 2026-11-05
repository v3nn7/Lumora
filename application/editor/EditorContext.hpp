#pragma once

#include "core/Uuid.hpp"

#include <entt/entity/entity.hpp>
#include <QObject>
#include <QString>

#include <functional>

namespace lumora {
class Scene;
}

namespace lumora::editor {

/// Shared editor state: the active scene (edit or play copy), the selection, and the
/// mutation entry points every panel uses so that change notifications stay consistent.
class EditorContext final : public QObject {
    Q_OBJECT
public:
    using EntityInit = std::function<void(Scene&, entt::entity)>;

    [[nodiscard]] Scene* scene() const noexcept { return scene_; }
    void setScene(Scene* scene, bool playing);
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }

    [[nodiscard]] Uuid selectedUuid() const noexcept { return selected_; }
    [[nodiscard]] entt::entity selectedEntity() const;
    void select(Uuid id);
    void clearSelection() { select(Uuid{}); }

    entt::entity createEntity(const QString& name, const EntityInit& init = {}, bool asChildOfSelection = false);
    void deleteSelected();
    void duplicateSelected();
    void renameEntity(Uuid id, const QString& name);
    /// Moves `child` under `parent` (null = root) at sibling position `index`.
    bool reparent(Uuid child, Uuid parent, int index);

    /// Call after editing component values. `source` lets the originating panel skip its own refresh.
    void notifyValuesChanged(QObject* source);
    void notifyStructureChanged();

    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }
    void setDirty(bool dirty);

signals:
    void sceneReplaced();
    void structureChanged();
    void selectionChanged(lumora::Uuid id);
    void valuesChanged(QObject* source);
    void dirtyChanged(bool dirty);

private:
    void markEdited();

    Scene* scene_ = nullptr;
    bool playing_ = false;
    Uuid selected_;
    bool dirty_ = false;
};

} // namespace lumora::editor
