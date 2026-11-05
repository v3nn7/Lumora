#include "scene/ScenePatch.hpp"

#include "core/Log.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <algorithm>

namespace lumora {

using nlohmann::json;

SceneIndex SceneIndex::fromDocument(const json& document) {
    SceneIndex index;
    index.name = document.value("name", std::string{});
    if (const auto it = document.find("environment"); it != document.end()) index.environment = *it;
    const auto entities = document.find("entities");
    if (entities == document.end() || !entities->is_array()) return index;
    std::unordered_map<std::string, std::size_t> siblings;  // parent uuid ("" = root) -> count
    for (const json& record : *entities) {
        if (!record.is_object() || !record.contains("uuid") || !record["uuid"].is_string()) continue;
        std::string uuid = record["uuid"].get<std::string>();
        const std::string parent = record.value("parent", std::string{});
        json withIndex = record;
        withIndex["index"] = siblings[parent]++;
        index.order.push_back(uuid);
        index.entities.emplace(std::move(uuid), std::move(withIndex));
    }
    return index;
}

SceneIndex SceneIndex::fromScene(const Scene& scene) { return fromDocument(SceneSerializer::toJson(scene)); }

bool ScenePatch::empty() const noexcept {
    return upserts.empty() && removals.empty() && !environment && !name;
}

ScenePatch ScenePatch::between(const SceneIndex& from, const SceneIndex& to) {
    ScenePatch patch;
    for (const std::string& uuid : from.order) {
        if (to.entities.contains(uuid)) continue;
        if (auto id = Uuid::parse(uuid)) patch.removals.push_back(*id);
    }
    for (const std::string& uuid : to.order) {
        const json& record = to.entities.at(uuid);
        const auto old = from.entities.find(uuid);
        if (old == from.entities.end() || old->second != record) patch.upserts.push_back(record);
    }
    if (from.environment != to.environment) patch.environment = to.environment;
    if (from.name != to.name) patch.name = to.name;
    return patch;
}

namespace {

/// Position of `entity` among its siblings.
std::size_t siblingIndex(const Scene& scene, entt::entity entity) {
    const entt::entity parent = scene.parentOf(entity);
    const auto& list = parent == entt::null ? scene.roots() : scene.childrenOf(parent);
    return static_cast<std::size_t>(std::ranges::find(list, entity) - list.begin());
}

} // namespace

Result<> ScenePatch::applyTo(Scene& scene) const {
    std::optional<Error> firstError;
    auto fail = [&](Error error) {
        log::warn("Scene", "patch: {}", error.message);
        if (!firstError) firstError = std::move(error);
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
        entt::entity parent = entt::null;
        if (const auto p = record.find("parent"); p != record.end() && p->is_string()) {
            auto parentId = Uuid::parse(p->get<std::string>());
            if (parentId) parent = scene.findByUuid(*parentId);
            if (!parentId || parent == entt::null) {
                log::debug("Scene", "patch: parent of {} is gone; attaching to the root", uuid->toString());
            }
        }
        const std::size_t index = record.contains("index") && record["index"].is_number_unsigned()
                                      ? record["index"].get<std::size_t>()
                                      : SIZE_MAX;
        const std::string entityName = record.contains("name") && record["name"].is_string()
                                     ? record["name"].get<std::string>()
                                     : std::string("Entity");
        entt::entity e = scene.findByUuid(*uuid);
        if (e == entt::null) {
            e = scene.createEntity(entityName, *uuid, parent);
            scene.setParent(e, parent, index);
        } else {
            if (scene.nameOf(e) != entityName) scene.rename(e, entityName);
            if (scene.parentOf(e) != parent || (index != SIZE_MAX && siblingIndex(scene, e) != index)) {
                if (!scene.setParent(e, parent, index)) {
                    fail(Error{"cannot move " + uuid->toString() + " under its own descendant"});
                }
            }
        }
        const auto components = record.find("components");
        SceneSerializer::applyComponents(scene, e, components != record.end() ? *components : json::object(), true);
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
