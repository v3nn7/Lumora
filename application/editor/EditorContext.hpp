#pragma once

#include "core/Uuid.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"

#include <entt/entity/entity.hpp>
#include <nlohmann/json_fwd.hpp>
#include <QObject>
#include <QString>
#include <QUndoStack>

#include <functional>

namespace lumora {
class Scene;
}

namespace lumora::editor {

/// Shared editor state: the active scene (edit or play copy), the selection, the undo history
/// and the mutation entry points every panel uses, so change notifications stay consistent.
///
/// While editing, every mutation is recorded on the undo stack. In play mode edits go straight
/// to the throwaway play copy and are neither recorded nor marked dirty.
class EditorContext final : public QObject {
    Q_OBJECT
public:
    using EntityInit = std::function<void(Scene&, entt::entity)>;

    EditorContext();

    [[nodiscard]] Scene* scene() const noexcept { return scene_; }
    void setScene(Scene* scene, bool playing);
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }

    [[nodiscard]] Uuid selectedUuid() const noexcept { return selected_; }
    [[nodiscard]] entt::entity selectedEntity() const;
    void select(Uuid id);
    void clearSelection() { select(Uuid{}); }

    // ---- Structural edits (recorded as scene snapshots) -----------------------------------
    entt::entity createEntity(const QString& name, const EntityInit& init = {}, bool asChildOfSelection = false);
    void deleteSelected();
    void duplicateSelected();
    void renameEntity(Uuid id, const QString& name);
    /// Moves `child` under `parent` (null = root) at sibling position `index`.
    bool reparent(Uuid child, Uuid parent, int index);
    void addComponent(Uuid id, const ComponentDesc& component);
    /// Arbitrary structural change recorded as one undo step (e.g. instantiating a model).
    void edit(const QString& text, const std::function<void(Scene&)>& change);
    void removeComponent(Uuid id, const ComponentDesc& component);

    // ---- Value edits (mergeable commands) ---------------------------------------------------
    void setField(Uuid id, const ComponentDesc& component, const FieldDesc& field, const FieldValue& value);
    void setEnvironment(const QString& setting, const std::function<void(EnvironmentSettings&)>& mutate);
    /// Records a transform change that has already been applied (e.g. at the end of a gizmo drag).
    void commitTransform(Uuid id, const QString& text, const TransformComponent& before);

    /// Called inside the recorded edit right after a component was added (e.g. to fit a collider).
    std::function<void(Scene&, entt::entity, const ComponentDesc&)> onComponentAdded;

    // ---- History ---------------------------------------------------------------------------
    [[nodiscard]] QUndoStack& undoStack() noexcept { return undo_; }
    /// Forgets all history (new scene loaded) and marks the current state as saved.
    void resetHistory();
    /// Used by snapshot commands: replaces the scene contents in place.
    void restoreSnapshot(const nlohmann::json& snapshot);

    /// Call after editing component values directly. `source` lets the originating panel skip its own refresh.
    void notifyValuesChanged(QObject* source);
    void notifyStructureChanged();

    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }
    void setDirty(bool dirty);
    /// Current state was written to disk.
    void markSaved();

signals:
    void sceneReplaced();
    void structureChanged();
    void selectionChanged(lumora::Uuid id);
    void valuesChanged(QObject* source);
    void dirtyChanged(bool dirty);

private:
    [[nodiscard]] bool recording() const noexcept { return scene_ != nullptr && !playing_; }
    /// Runs `edit`, recording a before/after scene snapshot when not in play mode.
    void structuralEdit(const QString& text, const std::function<void()>& edit);

    Scene* scene_ = nullptr;
    bool playing_ = false;
    Uuid selected_;
    bool dirty_ = false;
    QUndoStack undo_;
};

} // namespace lumora::editor
