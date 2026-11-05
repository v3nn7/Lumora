#include "editor/EditorContext.hpp"

#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <algorithm>

namespace lumora::editor {

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

void EditorContext::markEdited() {
    // Play-mode edits live in the throwaway play copy and never dirty the saved scene.
    if (!playing_) setDirty(true);
}

void EditorContext::setDirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    emit dirtyChanged(dirty);
}

entt::entity EditorContext::createEntity(const QString& name, const EntityInit& init, bool asChildOfSelection) {
    if (scene_ == nullptr) return entt::null;
    const entt::entity parent = asChildOfSelection ? selectedEntity() : entt::entity{entt::null};
    const entt::entity e = scene_->createEntity(name.toStdString(), Uuid::generate(), parent);
    if (init) init(*scene_, e);
    scene_->updateWorldTransforms();
    markEdited();
    emit structureChanged();
    select(scene_->uuidOf(e));
    return e;
}

void EditorContext::deleteSelected() {
    const entt::entity e = selectedEntity();
    if (e == entt::null) return;
    log::info("Editor", "deleted '{}'", scene_->nameOf(e));
    scene_->destroyEntity(e);
    selected_ = Uuid{};
    markEdited();
    emit structureChanged();
    emit selectionChanged(selected_);
}

void EditorContext::duplicateSelected() {
    const entt::entity e = selectedEntity();
    if (e == entt::null) return;
    const entt::entity copy = scene_->duplicateEntity(e);
    scene_->rename(copy, scene_->nameOf(e) + " Copy");
    scene_->updateWorldTransforms();
    markEdited();
    emit structureChanged();
    select(scene_->uuidOf(copy));
}

void EditorContext::renameEntity(Uuid id, const QString& name) {
    if (scene_ == nullptr) return;
    const entt::entity e = scene_->findByUuid(id);
    const std::string newName = name.trimmed().toStdString();
    if (e == entt::null || newName.empty() || scene_->nameOf(e) == newName) return;
    scene_->rename(e, newName);
    markEdited();
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
    if (!scene_->setParent(c, p, index < 0 ? SIZE_MAX : static_cast<std::size_t>(index))) {
        return false;
    }
    scene_->updateWorldTransforms();
    markEdited();
    emit structureChanged();
    return true;
}

void EditorContext::notifyValuesChanged(QObject* source) {
    if (scene_ != nullptr) scene_->updateWorldTransforms();
    markEdited();
    emit valuesChanged(source);
}

void EditorContext::notifyStructureChanged() {
    markEdited();
    emit structureChanged();
}

} // namespace lumora::editor
