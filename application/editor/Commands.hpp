#pragma once

#include "core/Uuid.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"

#include <nlohmann/json.hpp>
#include <QElapsedTimer>
#include <QUndoCommand>

#include <functional>

namespace lumora::editor {

class EditorContext;

enum class CommandId : int { Field = 1, Environment = 2, Transform = 3 };

/// Structural edits (create/delete/duplicate/reparent/rename, add/remove component): the scene
/// is captured before and after; undo/redo apply the per-entity difference (ScenePatch), so
/// entities the edit did not touch, including collaborators' concurrent changes, are kept.
class SceneSnapshotCommand final : public QUndoCommand {
public:
    SceneSnapshotCommand(EditorContext& ctx, const QString& text, nlohmann::json before, nlohmann::json after);
    void undo() override;
    void redo() override;

private:
    EditorContext& ctx_;
    nlohmann::json before_;
    nlohmann::json after_;
    bool applied_ = true;  // the edit already happened when the command was recorded
};

/// One reflected field on one or several entities (a multi-selection) set to one value.
/// Consecutive edits of the same field (spin-box scrubbing, slider drags, colour picking) within
/// a short window merge into a single undo step.
class FieldCommand final : public QUndoCommand {
public:
    FieldCommand(EditorContext& ctx, Uuid entity, const ComponentDesc& component, const FieldDesc& field,
                 FieldValue before, FieldValue after);
    FieldCommand(EditorContext& ctx, std::vector<Uuid> entities, const ComponentDesc& component, const FieldDesc& field,
                 std::vector<FieldValue> before, FieldValue after);
    void undo() override;
    void redo() override;
    [[nodiscard]] int id() const override { return static_cast<int>(CommandId::Field); }
    bool mergeWith(const QUndoCommand* other) override;

private:
    void apply(Uuid entity, const FieldValue& value);

    EditorContext& ctx_;
    std::vector<Uuid> entities_;
    const ComponentDesc& component_;
    const FieldDesc& field_;
    std::vector<FieldValue> before_;  // per entity
    FieldValue after_;
    QElapsedTimer age_;
};

/// Scene-wide environment edit; merges like FieldCommand per setting.
class EnvironmentCommand final : public QUndoCommand {
public:
    EnvironmentCommand(EditorContext& ctx, const QString& setting, EnvironmentSettings before, EnvironmentSettings after);
    void undo() override;
    void redo() override;
    [[nodiscard]] int id() const override { return static_cast<int>(CommandId::Environment); }
    bool mergeWith(const QUndoCommand* other) override;

private:
    void apply(const EnvironmentSettings& value);

    EditorContext& ctx_;
    QString setting_;
    EnvironmentSettings before_;
    EnvironmentSettings after_;
    QElapsedTimer age_;
};

/// Transform change from a gizmo drag; recorded after the drag already moved the entity.
class TransformCommand final : public QUndoCommand {
public:
    TransformCommand(EditorContext& ctx, const QString& text, Uuid entity, TransformComponent before,
                     TransformComponent after);
    void undo() override;
    void redo() override;

private:
    void apply(const TransformComponent& value);

    EditorContext& ctx_;
    Uuid entity_;
    TransformComponent before_;
    TransformComponent after_;
    bool applied_ = true;
};

} // namespace lumora::editor
