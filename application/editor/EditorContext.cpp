#include "editor/EditorContext.hpp"

#include "core/Log.hpp"
#include "editor/Commands.hpp"
#include "scene/Scene.hpp"
#include "scene/ScenePatch.hpp"
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
    pruneSelection();
    emit sceneReplaced();
    emit selectionChanged(selected_);
}

bool EditorContext::pruneSelection() {
    const std::size_t before = selection_.size();
    std::erase_if(selection_, [this](Uuid id) { return scene_ == nullptr || scene_->findByUuid(id) == entt::null; });
    const Uuid primary = selection_.empty() ? Uuid{} : selection_.back();
    const bool changed = selection_.size() != before || primary != selected_;
    selected_ = primary;
    return changed;
}

entt::entity EditorContext::selectedEntity() const {
    return scene_ != nullptr && selected_.isValid() ? scene_->findByUuid(selected_) : entt::entity{entt::null};
}

bool EditorContext::isSelected(Uuid id) const { return std::ranges::find(selection_, id) != selection_.end(); }

std::vector<entt::entity> EditorContext::selectedRoots() const {
    std::vector<entt::entity> roots;
    if (scene_ == nullptr) return roots;
    for (Uuid id : selection_) {
        const entt::entity e = scene_->findByUuid(id);
        if (e == entt::null) continue;
        bool underSelected = false;
        for (entt::entity p = scene_->parentOf(e); p != entt::null && !underSelected; p = scene_->parentOf(p)) {
            underSelected = isSelected(scene_->uuidOf(p));
        }
        if (!underSelected) roots.push_back(e);
    }
    return roots;
}

void EditorContext::select(Uuid id) {
    if (id == selected_ && selection_.size() == (id.isValid() ? 1u : 0u)) return;
    setSelection(id.isValid() ? std::vector<Uuid>{id} : std::vector<Uuid>{});
}

void EditorContext::toggleSelected(Uuid id) {
    if (!id.isValid()) return;
    std::vector<Uuid> next = selection_;
    if (const auto it = std::ranges::find(next, id); it != next.end()) {
        next.erase(it);
    } else {
        next.push_back(id);
    }
    setSelection(std::move(next));
}

void EditorContext::setSelection(std::vector<Uuid> ids) {
    std::erase_if(ids, [](Uuid id) { return !id.isValid(); });
    // No duplicates; the last occurrence is the primary.
    std::vector<Uuid> unique;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        if (std::ranges::find(unique, *it) == unique.end()) unique.insert(unique.begin(), *it);
    }
    if (unique == selection_ && (unique.empty() ? !selected_.isValid() : selected_ == unique.back())) return;
    selection_ = std::move(unique);
    selected_ = selection_.empty() ? Uuid{} : selection_.back();
    emit selectionChanged(selected_);
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

void EditorContext::applyPatch(const ScenePatch& patch) {
    if (scene_ == nullptr || patch.empty()) return;
    if (auto applied = patch.applyTo(*scene_); !applied) {
        log::error("Editor", "scene change only partly applied: {}", applied.error().message);
    }
    if (pruneSelection()) emit selectionChanged(selected_);
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
    const std::vector<entt::entity> roots = selectedRoots();
    if (roots.empty()) return;
    const QString name = roots.size() == 1 ? QString::fromStdString(scene_->nameOf(roots.front()))
                                           : tr("%n entities", nullptr, static_cast<int>(roots.size()));
    std::vector<Uuid> ids;
    for (entt::entity e : roots) ids.push_back(scene_->uuidOf(e));
    structuralEdit(tr("Delete %1").arg(name), [&] {
        for (Uuid id : ids) {
            const entt::entity e = scene_->findByUuid(id);
            if (e != entt::null) scene_->destroyEntity(e);
        }
    });
    log::info("Editor", "deleted {}", name.toStdString());
    selection_.clear();
    selected_ = Uuid{};
    emit structureChanged();
    emit selectionChanged(selected_);
}

void EditorContext::duplicateSelected() {
    const std::vector<entt::entity> roots = selectedRoots();
    if (roots.empty()) return;
    std::vector<Uuid> copies;
    const QString name = roots.size() == 1 ? QString::fromStdString(scene_->nameOf(roots.front()))
                                           : tr("%n entities", nullptr, static_cast<int>(roots.size()));
    structuralEdit(tr("Duplicate %1").arg(name), [&] {
        for (entt::entity e : roots) {
            const entt::entity copy = scene_->duplicateEntity(e);
            scene_->rename(copy, scene_->nameOf(e) + " Copy");
            copies.push_back(scene_->uuidOf(copy));
        }
        scene_->updateWorldTransforms();
    });
    emit structureChanged();
    setSelection(std::move(copies));
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
        if (onComponentAdded) onComponentAdded(*scene_, e, component);
        scene_->updateWorldTransforms();
    });
    emit structureChanged();
}

void EditorContext::edit(const QString& text, const std::function<void(Scene&)>& change) {
    if (scene_ == nullptr) return;
    structuralEdit(text, [&] {
        change(*scene_);
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

void EditorContext::setFieldOnSelection(const ComponentDesc& component, const FieldDesc& field, const FieldValue& value) {
    if (scene_ == nullptr) return;
    std::vector<Uuid> ids;
    std::vector<FieldValue> before;
    for (Uuid id : selection_) {
        const entt::entity e = scene_->findByUuid(id);
        if (e == entt::null || !component.has(scene_->registry(), e)) continue;
        if (!recording()) {
            field.set(scene_->registry(), e, value);
            continue;
        }
        FieldValue current = field.get(scene_->registry(), e);
        if (current == value) continue;
        ids.push_back(id);
        before.push_back(std::move(current));
    }
    if (!recording()) {
        notifyValuesChanged(nullptr);
        return;
    }
    if (!ids.empty()) undo_.push(new FieldCommand(*this, std::move(ids), component, field, std::move(before), value));
}

void EditorContext::commitTransforms(const QString& text, const std::vector<std::pair<Uuid, TransformComponent>>& before) {
    if (!recording() || before.empty()) return;
    undo_.beginMacro(text);
    for (const auto& [id, transform] : before) commitTransform(id, text, transform);
    undo_.endMacro();
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
