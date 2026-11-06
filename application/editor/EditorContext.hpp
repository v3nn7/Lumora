#pragma once

#include "common/ModuleExport.hpp"

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
struct ScenePatch;
}

namespace lumora::editor {

/// Shared editor state: the active scene (edit or play copy), the selection, the undo history
/// and the mutation entry points every panel uses, so change notifications stay consistent.
///
/// While editing, every mutation is recorded on the undo stack. In play mode edits go straight
/// to the throwaway play copy and are neither recorded nor marked dirty.
class LUMORA_EDITOR_API EditorContext final : public QObject {
    Q_OBJECT
public:
    using EntityInit = std::function<void(Scene&, entt::entity)>;

    EditorContext();

    [[nodiscard]] Scene* scene() const noexcept { return scene_; }
    void setScene(Scene* scene, bool playing);
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }

    /// The primary selection (the one the inspector and the gizmo follow).
    [[nodiscard]] Uuid selectedUuid() const noexcept { return selected_; }
    [[nodiscard]] entt::entity selectedEntity() const;
    /// Everything selected (the primary last); empty when nothing is.
    [[nodiscard]] const std::vector<Uuid>& selection() const noexcept { return selection_; }
    [[nodiscard]] bool isSelected(Uuid id) const;
    /// Selected entities that are not under another selected one (what moves, copies, deletes).
    [[nodiscard]] std::vector<entt::entity> selectedRoots() const;
    void select(Uuid id);
    /// Ctrl+click: adds an entity (making it primary) or removes it.
    void toggleSelected(Uuid id);
    void setSelection(std::vector<Uuid> ids);
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
    /// The field on every selected entity that has the component (one undo step).
    void setFieldOnSelection(const ComponentDesc& component, const FieldDesc& field, const FieldValue& value);
    /// Several transforms changed together (a group moved with the gizmo): one undo step.
    void commitTransforms(const QString& text, const std::vector<std::pair<Uuid, TransformComponent>>& before);
    void setEnvironment(const QString& setting, const std::function<void(EnvironmentSettings&)>& mutate);
    /// Records a transform change that has already been applied (e.g. at the end of a gizmo drag).
    void commitTransform(Uuid id, const QString& text, const TransformComponent& before);

    /// Called inside the recorded edit right after a component was added (e.g. to fit a collider).
    std::function<void(Scene&, entt::entity, const ComponentDesc&)> onComponentAdded;

    // ---- History ---------------------------------------------------------------------------
    [[nodiscard]] QUndoStack& undoStack() noexcept { return undo_; }
    /// Forgets all history (new scene loaded) and marks the current state as saved.
    void resetHistory();
    /// Applies a scene patch in place (undo/redo of structural edits, collaborators' edits):
    /// only the entities in the patch change. Keeps the selection valid and notifies panels.
    void applyPatch(const ScenePatch& patch);

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
    /// Drops selected entities that no longer exist; true if the selection changed.
    bool pruneSelection();
    [[nodiscard]] bool recording() const noexcept { return scene_ != nullptr && !playing_; }
    /// Runs `edit`, recording a before/after scene snapshot when not in play mode.
    void structuralEdit(const QString& text, const std::function<void()>& edit);

    Scene* scene_ = nullptr;
    bool playing_ = false;
    Uuid selected_;
    std::vector<Uuid> selection_;
    bool dirty_ = false;
    QUndoStack undo_;
};

} // namespace lumora::editor
