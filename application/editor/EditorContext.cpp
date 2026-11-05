#include "editor/EditorContext.hpp"

#include "core/Log.hpp"
#include "editor/Commands.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace lumora::editor {

EditorContext::EditorContext() {
    undo_.setUndoLimit(256);
    connect(&undo_, &QUndoStack::cleanChanged, this, [this](bool clean) {
        if (!playing_) setDirty(!clean);
    });
}

void EditorContext::setScene(Scene* scene, bool playing) {
    scene_ = scene;
    playing_ = playing;
    if (scene_ == nullptr || (selected_.isValid() && scene_->findByUuid(selected_) == entt::null)) {
        selected_ = Uuid{};
    }
    emit sceneReplaced();
    emit selectionChanged(selected_);
}

entt::entity EditorContext::selectedEntity() const {
    return scene_ != nullptr && selected_.isValid() ? scene_->findByUuid(selected_) : entt::entity{entt::null};
}

void EditorContext::select(Uuid id) {
    if (id == selected_) return;
    selected_ = id;
    emit selectionChanged(id);
}

void EditorContext::setDirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    emit dirtyChanged(dirty);
}

void EditorContext::markSaved() {
    undo_.setClean();
    setDirty(false);
}

void EditorContext::resetHistory() {
    undo_.clear();
    markSaved();
}

void EditorContext::structuralEdit(const QString& text, const std::function<void()>& edit) {
    if (!recording()) {
        edit();
        return;
    }
    nlohmann::json before = SceneSerializer::toJson(*scene_);
    edit();
    nlohmann::json after = SceneSerializer::toJson(*scene_);
    if (after == before) return;  // nothing changed (e.g. rejected re-parent): no empty undo step
    undo_.push(new SceneSnapshotCommand(*this, text, std::move(before), std::move(after)));
}

void EditorContext::restoreSnapshot(const nlohmann::json& snapshot) {
    if (scene_ == nullptr) return;
    if (auto loaded = SceneSerializer::loadInto(*scene_, snapshot); !loaded) {
        log::error("Editor", "undo/redo failed to restore the scene: {}", loaded.error().message);
    }
    if (selected_.isValid() && scene_->findByUuid(selected_) == entt::null) {
        selected_ = Uuid{};
        emit selectionChanged(selected_);
    }
    emit structureChanged();
    emit valuesChanged(nullptr);
}

entt::entity EditorContext::createEntity(const QString& name, const EntityInit& init, bool asChildOfSelection) {
    if (scene_ == nullptr) return entt::null;
    entt::entity created = entt::null;
    structuralEdit(tr("Create %1").arg(name), [&] {
        const entt::entity parent = asChildOfSelection ? selectedEntity() : entt::entity{entt::null};
        created = scene_->createEntity(name.toStdString(), Uuid::generate(), parent);
        if (init) init(*scene_, created);
        scene_->updateWorldTransforms();
    });
    emit structureChanged();
    select(scene_->uuidOf(created));
    return created;
}

void EditorContext::deleteSelected() {
    const entt::entity e = selectedEntity();
    if (e == entt::null) return;
    const QString name = QString::fromStdString(scene_->nameOf(e));
    structuralEdit(tr("Delete %1").arg(name), [&] { scene_->destroyEntity(e); });
    log::info("Editor", "deleted '{}'", name.toStdString());
    selected_ = Uuid{};
    emit structureChanged();
    emit selectionChanged(selected_);
}

void EditorContext::duplicateSelected() {
    const entt::entity e = selectedEntity();
    if (e == entt::null) return;
    entt::entity copy = entt::null;
    structuralEdit(tr("Duplicate %1").arg(QString::fromStdString(scene_->nameOf(e))), [&] {
        copy = scene_->duplicateEntity(e);
        scene_->rename(copy, scene_->nameOf(e) + " Copy");
        scene_->updateWorldTransforms();
    });
    emit structureChanged();
    select(scene_->uuidOf(copy));
}

void EditorContext::renameEntity(Uuid id, const QString& name) {
    if (scene_ == nullptr) return;
    const entt::entity e = scene_->findByUuid(id);
    const std::string newName = name.trimmed().toStdString();
    if (e == entt::null || newName.empty() || scene_->nameOf(e) == newName) return;
    structuralEdit(tr("Rename to %1").arg(name.trimmed()), [&] { scene_->rename(e, newName); });
    emit structureChanged();
    emit valuesChanged(nullptr);
}

bool EditorContext::reparent(Uuid child, Uuid parent, int index) {
    if (scene_ == nullptr) return false;
    const entt::entity c = scene_->findByUuid(child);
    const entt::entity p = parent.isValid() ? scene_->findByUuid(parent) : entt::entity{entt::null};
    if (c == entt::null) return false;
    // Moving forward within the same sibling list: removal shifts the target index by one.
    if (scene_->parentOf(c) == p && index >= 0) {
        const auto& siblings = p == entt::null ? scene_->roots() : scene_->childrenOf(p);
        const auto it = std::ranges::find(siblings, c);
        if (it != siblings.end() && static_cast<int>(it - siblings.begin()) < index) --index;
    }
    bool moved = false;
    structuralEdit(tr("Reparent %1").arg(QString::fromStdString(scene_->nameOf(c))), [&] {
        moved = scene_->setParent(c, p, index < 0 ? SIZE_MAX : static_cast<std::size_t>(index));
        scene_->updateWorldTransforms();
    });
    emit structureChanged();
    return moved;
}

void EditorContext::addComponent(Uuid id, const ComponentDesc& component) {
    const entt::entity e = scene_ ? scene_->findByUuid(id) : entt::entity{entt::null};
    if (e == entt::null || component.has(scene_->registry(), e)) return;
    structuralEdit(tr("Add %1").arg(QString::fromStdString(component.label)), [&] {
        component.add(scene_->registry(), e);
        scene_->updateWorldTransforms();
    });
    emit structureChanged();
}

void EditorContext::removeComponent(Uuid id, const ComponentDesc& component) {
    const entt::entity e = scene_ ? scene_->findByUuid(id) : entt::entity{entt::null};
    if (e == entt::null || !component.has(scene_->registry(), e) || !component.removable) return;
    structuralEdit(tr("Remove %1").arg(QString::fromStdString(component.label)),
                   [&] { component.remove(scene_->registry(), e); });
    emit structureChanged();
}

void EditorContext::setField(Uuid id, const ComponentDesc& component, const FieldDesc& field, const FieldValue& value) {
    const entt::entity e = scene_ ? scene_->findByUuid(id) : entt::entity{entt::null};
    if (e == entt::null || !component.has(scene_->registry(), e)) return;
    if (!recording()) {
        field.set(scene_->registry(), e, value);
        notifyValuesChanged(nullptr);
        return;
    }
    const FieldValue before = field.get(scene_->registry(), e);
    if (before == value) return;
    undo_.push(new FieldCommand(*this, id, component, field, before, value));  // push() applies it
}

void EditorContext::setEnvironment(const QString& setting, const std::function<void(EnvironmentSettings&)>& mutate) {
    if (scene_ == nullptr) return;
    if (!recording()) {
        mutate(scene_->environment());
        notifyValuesChanged(nullptr);
        return;
    }
    const EnvironmentSettings before = scene_->environment();
    EnvironmentSettings after = before;
    mutate(after);
    undo_.push(new EnvironmentCommand(*this, setting, before, after));
}

void EditorContext::commitTransform(Uuid id, const QString& text, const TransformComponent& before) {
    const entt::entity e = scene_ ? scene_->findByUuid(id) : entt::entity{entt::null};
    if (e == entt::null) return;
    const TransformComponent& after = scene_->registry().get<TransformComponent>(e);
    if (!recording() || (after.position == before.position && after.rotationDeg == before.rotationDeg &&
                         after.scale == before.scale)) {
        return;
    }
    undo_.push(new TransformCommand(*this, text, id, before, after));
}

void EditorContext::notifyValuesChanged(QObject* source) {
    if (scene_ != nullptr) scene_->updateWorldTransforms();
    emit valuesChanged(source);
}

void EditorContext::notifyStructureChanged() { emit structureChanged(); }

} // namespace lumora::editor
