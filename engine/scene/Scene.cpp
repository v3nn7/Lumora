#include "scene/Scene.hpp"

#include "core/Assert.hpp"
#include "core/Log.hpp"

#include <algorithm>

namespace lumora {

namespace {
const std::vector<entt::entity> kNoChildren;
const std::string kNoName;
} // namespace

Scene::Scene(std::string name) : name_(std::move(name)) {}

entt::entity Scene::createEntity(std::string_view name, Uuid id, entt::entity parent) {
    if (!id.isValid() || uuidMap_.contains(id)) {
        if (id.isValid()) {
            log::warn("Scene", "duplicate UUID {} - generating a new one", id.toString());
        }
        id = Uuid::generate();
    }
    const entt::entity e = registry_.create();
    registry_.emplace<IdComponent>(e, id);
    registry_.emplace<NameComponent>(e, std::string(name));
    registry_.emplace<TransformComponent>(e);
    registry_.emplace<WorldTransformComponent>(e);
    registry_.emplace<HierarchyComponent>(e);
    uuidMap_.emplace(id, e);
    roots_.push_back(e);
    if (parent != entt::null) {
        setParent(e, parent);
    }
    return e;
}

void Scene::destroyEntity(entt::entity entity) {
    if (!registry_.valid(entity)) {
        return;
    }
    // Copy: children vector is mutated by recursive detach.
    const std::vector<entt::entity> children = childrenOf(entity);
    for (entt::entity child : children) {
        destroyEntity(child);
    }
    detach(entity);
    std::erase(roots_, entity);
    uuidMap_.erase(registry_.get<IdComponent>(entity).id);
    registry_.destroy(entity);
}

void Scene::clear() {
    registry_.clear();
    uuidMap_.clear();
    roots_.clear();
    environment_ = EnvironmentSettings{};
}

void Scene::detach(entt::entity entity) {
    auto& h = registry_.get<HierarchyComponent>(entity);
    if (h.parent != entt::null && registry_.valid(h.parent)) {
        std::erase(registry_.get<HierarchyComponent>(h.parent).children, entity);
    } else {
        std::erase(roots_, entity);
    }
    h.parent = entt::null;
}

bool Scene::setParent(entt::entity child, entt::entity parent, std::size_t index) {
    if (!registry_.valid(child) || child == parent) {
        return false;
    }
    if (parent != entt::null && (!registry_.valid(parent) || isAncestor(child, parent))) {
        return false;
    }
    detach(child);
    std::vector<entt::entity>& list =
        parent == entt::null ? roots_ : registry_.get<HierarchyComponent>(parent).children;
    index = std::min(index, list.size());
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), child);
    registry_.get<HierarchyComponent>(child).parent = parent;
    return true;
}

entt::entity Scene::parentOf(entt::entity entity) const {
    if (!registry_.valid(entity)) return entt::null;
    return registry_.get<HierarchyComponent>(entity).parent;
}

const std::vector<entt::entity>& Scene::childrenOf(entt::entity entity) const {
    if (!registry_.valid(entity)) return kNoChildren;
    return registry_.get<HierarchyComponent>(entity).children;
}

bool Scene::isAncestor(entt::entity ancestor, entt::entity entity) const {
    for (entt::entity p = parentOf(entity); p != entt::null; p = parentOf(p)) {
        if (p == ancestor) return true;
    }
    return false;
}

entt::entity Scene::findByUuid(Uuid id) const {
    const auto it = uuidMap_.find(id);
    return it == uuidMap_.end() ? entt::null : it->second;
}

Uuid Scene::uuidOf(entt::entity entity) const {
    if (!registry_.valid(entity)) return Uuid{};
    return registry_.get<IdComponent>(entity).id;
}

const std::string& Scene::nameOf(entt::entity entity) const {
    if (!registry_.valid(entity)) return kNoName;
    return registry_.get<NameComponent>(entity).name;
}

void Scene::rename(entt::entity entity, std::string name) {
    if (registry_.valid(entity)) {
        registry_.get<NameComponent>(entity).name = std::move(name);
    }
}

void Scene::updateWorldTransforms() {
    for (entt::entity root : roots_) {
        updateWorldRecursive(root, glm::mat4(1.0f));
    }
}

void Scene::updateWorldRecursive(entt::entity entity, const glm::mat4& parentWorld) {
    const glm::mat4 world = parentWorld * registry_.get<TransformComponent>(entity).localMatrix();
    registry_.get<WorldTransformComponent>(entity).matrix = world;
    for (entt::entity child : registry_.get<HierarchyComponent>(entity).children) {
        updateWorldRecursive(child, world);
    }
}

glm::mat4 Scene::worldMatrix(entt::entity entity) const {
    glm::mat4 m(1.0f);
    for (entt::entity e = entity; e != entt::null && registry_.valid(e); e = parentOf(e)) {
        m = registry_.get<TransformComponent>(e).localMatrix() * m;
    }
    return m;
}

entt::entity Scene::primaryCamera() const {
    entt::entity fallback = entt::null;
    entt::entity found = entt::null;
    forEachDepthFirst([&](entt::entity e, int) {
        if (found != entt::null) return;
        if (const auto* cam = registry_.try_get<CameraComponent>(e)) {
            if (cam->primary) {
                found = e;
            } else if (fallback == entt::null) {
                fallback = e;
            }
        }
    });
    return found != entt::null ? found : fallback;
}

bool Scene::changeUuid(entt::entity entity, Uuid id) {
    if (!registry_.valid(entity) || !id.isValid()) return false;
    auto& current = registry_.get<IdComponent>(entity).id;
    if (current == id) return true;
    if (uuidMap_.contains(id)) return false;
    uuidMap_.erase(current);
    current = id;
    uuidMap_.emplace(id, entity);
    return true;
}

entt::entity Scene::duplicateEntity(entt::entity entity) {
    if (!registry_.valid(entity)) return entt::null;
    const entt::entity parent = parentOf(entity);
    const entt::entity copy = cloneRecursive(entity, parent);
    // Place the copy directly after the original among its siblings.
    const auto& siblings = parent == entt::null ? roots_ : childrenOf(parent);
    const auto it = std::find(siblings.begin(), siblings.end(), entity);
    setParent(copy, parent, static_cast<std::size_t>(it - siblings.begin()) + 1);
    return copy;
}

entt::entity Scene::cloneRecursive(entt::entity source, entt::entity newParent) {
    const entt::entity copy = createEntity(nameOf(source), Uuid::generate(), newParent);
    registry_.get<TransformComponent>(copy) = registry_.get<TransformComponent>(source);
    // Copy every other storage that holds a component for 'source', except identity/graph data.
    for (auto [id, storage] : registry_.storage()) {
        if (id == entt::type_id<IdComponent>().hash() || id == entt::type_id<HierarchyComponent>().hash() ||
            id == entt::type_id<NameComponent>().hash() || id == entt::type_id<TransformComponent>().hash() ||
            id == entt::type_id<WorldTransformComponent>().hash()) {
            continue;
        }
        if (storage.contains(source) && !storage.contains(copy)) {
            storage.push(copy, storage.value(source));
        }
    }
    const std::vector<entt::entity> children = childrenOf(source);
    for (entt::entity child : children) {
        cloneRecursive(child, copy);
    }
    return copy;
}

} // namespace lumora
