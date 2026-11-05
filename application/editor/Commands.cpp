#include "editor/Commands.hpp"

#include "editor/EditorContext.hpp"
#include "scene/Scene.hpp"
#include "scene/ScenePatch.hpp"

namespace lumora::editor {

namespace {
constexpr qint64 kMergeWindowMs = 1000;
}

SceneSnapshotCommand::SceneSnapshotCommand(EditorContext& ctx, const QString& text, nlohmann::json before,
                                           nlohmann::json after)
    : QUndoCommand(text), ctx_(ctx), before_(std::move(before)), after_(std::move(after)) {}

void SceneSnapshotCommand::undo() {
    ctx_.applyPatch(ScenePatch::between(SceneIndex::fromDocument(after_), SceneIndex::fromDocument(before_)));
    applied_ = false;
}

void SceneSnapshotCommand::redo() {
    if (applied_) return;  // first redo() comes from QUndoStack::push right after the edit
    ctx_.applyPatch(ScenePatch::between(SceneIndex::fromDocument(before_), SceneIndex::fromDocument(after_)));
    applied_ = true;
}

FieldCommand::FieldCommand(EditorContext& ctx, Uuid entity, const ComponentDesc& component, const FieldDesc& field,
                           FieldValue before, FieldValue after)
    : QUndoCommand(QObject::tr("Edit %1 %2")
                       .arg(QString::fromStdString(component.label), QString::fromStdString(field.label))),
      ctx_(ctx), entity_(entity), component_(component), field_(field), before_(std::move(before)),
      after_(std::move(after)) {
    age_.start();
}

void FieldCommand::apply(const FieldValue& value) {
    Scene* scene = ctx_.scene();
    const entt::entity e = scene ? scene->findByUuid(entity_) : entt::entity{entt::null};
    if (e == entt::null || !component_.has(scene->registry(), e)) return;
    field_.set(scene->registry(), e, value);
    ctx_.notifyValuesChanged(nullptr);
}

void FieldCommand::undo() { apply(before_); }
void FieldCommand::redo() { apply(after_); }

bool FieldCommand::mergeWith(const QUndoCommand* other) {
    const auto* o = static_cast<const FieldCommand*>(other);
    if (o->entity_ != entity_ || &o->field_ != &field_ || age_.elapsed() > kMergeWindowMs) return false;
    after_ = o->after_;
    age_.restart();
    return true;
}

EnvironmentCommand::EnvironmentCommand(EditorContext& ctx, const QString& setting, EnvironmentSettings before,
                                       EnvironmentSettings after)
    : QUndoCommand(QObject::tr("Edit %1").arg(setting)), ctx_(ctx), setting_(setting), before_(before), after_(after) {
    age_.start();
}

void EnvironmentCommand::apply(const EnvironmentSettings& value) {
    if (Scene* scene = ctx_.scene()) {
        scene->environment() = value;
        ctx_.notifyValuesChanged(nullptr);
    }
}

void EnvironmentCommand::undo() { apply(before_); }
void EnvironmentCommand::redo() { apply(after_); }

bool EnvironmentCommand::mergeWith(const QUndoCommand* other) {
    const auto* o = static_cast<const EnvironmentCommand*>(other);
    if (o->setting_ != setting_ || age_.elapsed() > kMergeWindowMs) return false;
    after_ = o->after_;
    age_.restart();
    return true;
}

TransformCommand::TransformCommand(EditorContext& ctx, const QString& text, Uuid entity, TransformComponent before,
                                   TransformComponent after)
    : QUndoCommand(text), ctx_(ctx), entity_(entity), before_(before), after_(after) {}

void TransformCommand::apply(const TransformComponent& value) {
    Scene* scene = ctx_.scene();
    const entt::entity e = scene ? scene->findByUuid(entity_) : entt::entity{entt::null};
    if (e == entt::null) return;
    scene->registry().get<TransformComponent>(e) = value;
    ctx_.notifyValuesChanged(nullptr);
}

void TransformCommand::undo() {
    apply(before_);
    applied_ = false;
}

void TransformCommand::redo() {
    if (applied_) return;
    apply(after_);
    applied_ = true;
}

} // namespace lumora::editor
