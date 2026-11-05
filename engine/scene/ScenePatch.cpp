#include "scene/ScenePatch.hpp"

#include "core/Log.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <unordered_set>

namespace lumora {

using nlohmann::json;

SceneIndex SceneIndex::fromDocument(const json& document) {
    SceneIndex index;
    index.name = document.value("name", std::string{});
    if (const auto it = document.find("environment"); it != document.end() && it->is_object()) {
        index.environment = *it;
    }
    const auto entities = document.find("entities");
    if (entities == document.end() || !entities->is_array()) return index;
    for (const json& record : *entities) {
        if (!record.is_object() || !record.contains("uuid") || !record["uuid"].is_string()) continue;
        std::string uuid = record["uuid"].get<std::string>();
        const std::string parent = record.contains("parent") && record["parent"].is_string()
                                       ? record["parent"].get<std::string>()
                                       : std::string{};
        index.children[parent].push_back(uuid);
        index.parents[uuid] = parent;
        index.order.push_back(uuid);
        index.entities.emplace(std::move(uuid), record);
    }
    return index;
}

SceneIndex SceneIndex::fromScene(const Scene& scene) { return fromDocument(SceneSerializer::toJson(scene)); }

std::size_t SceneIndex::positionOf(const std::string& uuid) const {
    const auto parent = parents.find(uuid);
    if (parent == parents.end()) return 0;
    const auto& list = children.at(parent->second);
    return static_cast<std::size_t>(std::ranges::find(list, uuid) - list.begin());
}

bool ScenePatch::empty() const noexcept {
    return upserts.empty() && removals.empty() && !environment && !name;
}

namespace {

/// Siblings that kept their parent but must move so the relative order matches `to`: everything
/// outside a longest run that is already in order (moving those few is enough).
std::unordered_set<std::string> reordered(const SceneIndex& from, const SceneIndex& to) {
    std::unordered_set<std::string> moved;
    for (const auto& [parent, siblings] : to.children) {
        std::vector<std::string> kept;  // in `to` order, same parent before and after
        std::vector<std::size_t> before;
        for (const std::string& uuid : siblings) {
            const auto p = from.parents.find(uuid);
            if (p == from.parents.end() || p->second != parent) continue;
            kept.push_back(uuid);
            before.push_back(from.positionOf(uuid));
        }
        // Longest increasing subsequence of the old positions (patience sorting, O(n log n)).
        std::vector<std::size_t> tails;     // index into `kept` of the smallest tail per length
        std::vector<std::ptrdiff_t> prev(kept.size(), -1);
        for (std::size_t i = 0; i < kept.size(); ++i) {
            const auto it = std::lower_bound(tails.begin(), tails.end(), before[i],
                                             [&](std::size_t k, std::size_t value) { return before[k] < value; });
            if (it != tails.begin()) prev[i] = static_cast<std::ptrdiff_t>(*(it - 1));
            if (it == tails.end()) {
                tails.push_back(i);
            } else {
                *it = i;
            }
        }
        std::vector<bool> inOrder(kept.size(), false);
        for (std::ptrdiff_t i = tails.empty() ? -1 : static_cast<std::ptrdiff_t>(tails.back()); i >= 0; i = prev[i]) {
            inOrder[static_cast<std::size_t>(i)] = true;
        }
        for (std::size_t i = 0; i < kept.size(); ++i) {
            if (!inOrder[i]) moved.insert(kept[i]);
        }
    }
    return moved;
}

json fieldChanges(const json& before, const json& after) {
    json changed = json::object();
    for (const auto& [field, value] : after.items()) {
        const auto old = before.find(field);
        if (old == before.end() || *old != value) changed[field] = value;
    }
    return changed;
}

} // namespace

ScenePatch ScenePatch::between(const SceneIndex& from, const SceneIndex& to) {
    ScenePatch patch;
    for (const std::string& uuid : from.order) {
        if (to.entities.contains(uuid)) continue;
        if (auto id = Uuid::parse(uuid)) patch.removals.push_back(*id);
    }
    const std::unordered_set<std::string> moved = reordered(from, to);
    for (const std::string& uuid : to.order) {
        const json& record = to.entities.at(uuid);
        const std::string& parent = to.parents.at(uuid);
        const auto old = from.entities.find(uuid);
        if (old == from.entities.end()) {
            json full = record;
            full["full"] = true;
            full["index"] = to.positionOf(uuid);
            patch.upserts.push_back(std::move(full));
            continue;
        }
        json partial = {{"uuid", uuid}};
        if (old->second.value("name", std::string{}) != record.value("name", std::string{})) {
            partial["name"] = record.value("name", std::string{});
        }
        if (from.parents.at(uuid) != parent || moved.contains(uuid)) {
            partial["move"] = {{"parent", parent.empty() ? json(nullptr) : json(parent)}, {"index", to.positionOf(uuid)}};
        }
        const json empty = json::object();
        const json& now = record.contains("components") ? record["components"] : empty;
        const json& was = old->second.contains("components") ? old->second["components"] : empty;
        json components = json::object();
        json added = json::array();
        for (const auto& [id, fields] : now.items()) {
            const auto previous = was.find(id);
            if (previous == was.end()) {
                components[id] = fields;
                added.push_back(id);
                continue;
            }
            json changed = fieldChanges(*previous, fields);
            if (!changed.empty()) components[id] = std::move(changed);
        }
        json removed = json::array();
        for (const auto& [id, fields] : was.items()) {
            if (!now.contains(id)) removed.push_back(id);
        }
        if (!components.empty()) partial["components"] = std::move(components);
        if (!added.empty()) partial["added"] = std::move(added);
        if (!removed.empty()) partial["removed"] = std::move(removed);
        if (partial.size() > 1) patch.upserts.push_back(std::move(partial));
    }
    json environment = fieldChanges(from.environment, to.environment);
    if (!environment.empty()) patch.environment = std::move(environment);
    if (from.name != to.name) patch.name = to.name;
    return patch;
}

namespace {

std::size_t siblingIndex(const Scene& scene, entt::entity entity) {
    const entt::entity parent = scene.parentOf(entity);
    const auto& list = parent == entt::null ? scene.roots() : scene.childrenOf(parent);
    return static_cast<std::size_t>(std::ranges::find(list, entity) - list.begin());
}

entt::entity resolveParent(const Scene& scene, const json& parent, const Uuid& child) {
    if (!parent.is_string()) return entt::null;
    auto id = Uuid::parse(parent.get<std::string>());
    const entt::entity e = id ? scene.findByUuid(*id) : entt::entity{entt::null};
    if (e == entt::null) log::debug("Scene", "patch: parent of {} is gone; attaching to the root", child.toString());
    return e;
}

std::size_t indexOf(const json& j) {
    return j.is_number_unsigned() ? j.get<std::size_t>() : SIZE_MAX;
}

} // namespace

Result<> ScenePatch::applyTo(Scene& scene) const {
    std::optional<Error> firstError;
    auto fail = [&](Error error) {
        log::warn("Scene", "patch: {}", error.message);
        if (!firstError) firstError = std::move(error);
    };
    auto place = [&](entt::entity e, entt::entity parent, std::size_t index, const Uuid& id) {
        if (scene.parentOf(e) == parent && (index == SIZE_MAX || siblingIndex(scene, e) == index)) return;
        if (!scene.setParent(e, parent, index)) fail(Error{"cannot move " + id.toString() + " under its own descendant"});
    };

    for (const Uuid& id : removals) {
        const entt::entity e = scene.findByUuid(id);
        if (e != entt::null) scene.destroyEntity(e);  // removing a parent already took its children
    }

    for (const json& record : upserts) {
        if (!record.is_object()) {
            fail(Error{"entity record is not an object"});
            continue;
        }
        auto uuid = Uuid::parse(record.value("uuid", std::string{}));
        if (!uuid) {
            fail(uuid.error());
            continue;
        }
        entt::entity e = scene.findByUuid(*uuid);
        const auto nameIt = record.find("name");
        const bool hasName = nameIt != record.end() && nameIt->is_string();

        if (record.value("full", false)) {
            const entt::entity parent = resolveParent(scene, record.value("parent", json()), *uuid);
            const std::string entityName = hasName ? nameIt->get<std::string>() : std::string("Entity");
            if (e == entt::null) {
                e = scene.createEntity(entityName, *uuid, parent);
            } else if (scene.nameOf(e) != entityName) {
                scene.rename(e, entityName);
            }
            place(e, parent, indexOf(record.value("index", json())), *uuid);
            const auto components = record.find("components");
            SceneSerializer::applyComponents(scene, e, components != record.end() ? *components : json::object(), true);
            continue;
        }

        if (e == entt::null) continue;  // edited here, deleted elsewhere: the deletion wins
        if (hasName && scene.nameOf(e) != nameIt->get<std::string>()) scene.rename(e, nameIt->get<std::string>());
        if (const auto move = record.find("move"); move != record.end() && move->is_object()) {
            place(e, resolveParent(scene, move->value("parent", json()), *uuid), indexOf(move->value("index", json())),
                  *uuid);
        }
        if (const auto components = record.find("components"); components != record.end() && components->is_object()) {
            const json added = record.value("added", json::array());
            for (const auto& [id, fields] : components->items()) {
                const ComponentDesc* desc = ComponentRegistry::builtin().find(id);
                if (desc == nullptr) continue;
                const bool adding = added.is_array() && std::ranges::find(added, json(id)) != added.end();
                if (!adding && !desc->has(scene.registry(), e)) continue;  // removed elsewhere meanwhile
                SceneSerializer::applyComponents(scene, e, json{{id, fields}}, false);
            }
        }
        if (const auto removed = record.find("removed"); removed != record.end() && removed->is_array()) {
            for (const json& id : *removed) {
                if (!id.is_string()) continue;
                const ComponentDesc* desc = ComponentRegistry::builtin().find(id.get<std::string>());
                if (desc != nullptr && desc->removable && desc->has(scene.registry(), e)) {
                    desc->remove(scene.registry(), e);
                }
            }
        }
    }

    if (environment) SceneSerializer::applyEnvironment(scene.environment(), *environment);
    if (name) scene.setName(*name);
    scene.updateWorldTransforms();
    if (firstError) return std::unexpected(*firstError);
    return {};
}

std::vector<Uuid> ScenePatch::touched() const {
    std::vector<Uuid> ids = removals;
    for (const json& record : upserts) {
        if (!record.is_object()) continue;
        if (auto id = Uuid::parse(record.value("uuid", std::string{}))) ids.push_back(*id);
    }
    return ids;
}

json ScenePatch::toJson() const {
    json out = json::object();
    out["upserts"] = upserts;
    json removed = json::array();
    for (const Uuid& id : removals) removed.push_back(id.toString());
    out["removals"] = std::move(removed);
    if (environment) out["environment"] = *environment;
    if (name) out["name"] = *name;
    return out;
}

Result<ScenePatch> ScenePatch::fromJson(const json& j) {
    if (!j.is_object()) return makeError("patch is not an object");
    ScenePatch patch;
    if (const auto it = j.find("upserts"); it != j.end()) {
        if (!it->is_array()) return makeError("patch upserts must be an array");
        for (const json& record : *it) {
            if (!record.is_object() || !record.contains("uuid") || !record["uuid"].is_string()) {
                return makeError("patch upsert without a uuid");
            }
            patch.upserts.push_back(record);
        }
    }
    if (const auto it = j.find("removals"); it != j.end()) {
        if (!it->is_array()) return makeError("patch removals must be an array");
        for (const json& id : *it) {
            if (!id.is_string()) return makeError("patch removal is not a string");
            auto uuid = Uuid::parse(id.get<std::string>());
            if (!uuid) return std::unexpected(uuid.error());
            patch.removals.push_back(*uuid);
        }
    }
    if (const auto it = j.find("environment"); it != j.end()) {
        if (!it->is_object()) return makeError("patch environment must be an object");
        patch.environment = *it;
    }
    if (const auto it = j.find("name"); it != j.end()) {
        if (!it->is_string()) return makeError("patch name must be a string");
        patch.name = it->get<std::string>();
    }
    return patch;
}

} // namespace lumora
