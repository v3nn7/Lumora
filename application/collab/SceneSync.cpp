#include "collab/SceneSync.hpp"

#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <algorithm>

namespace lumora::collab {

using nlohmann::json;

namespace {

constexpr const char* kSceneKey = "#scene";

/// Fine-grained keys of what a patch changes: "full", "removed", "name", "move", "c:<component>"
/// (added or removed) and "f:<component>.<field>", grouped by entity uuid. Environment settings
/// and the scene name live under kSceneKey.
std::vector<std::pair<std::string, std::string>> keysOf(const ScenePatch& patch) {
    std::vector<std::pair<std::string, std::string>> keys;
    for (const Uuid& id : patch.removals) keys.emplace_back(id.toString(), "removed");
    for (const json& record : patch.upserts) {
        const std::string uuid = record.value("uuid", std::string{});
        if (record.value("full", false)) {
            keys.emplace_back(uuid, "full");
            continue;
        }
        if (record.contains("name")) keys.emplace_back(uuid, "name");
        if (record.contains("move")) keys.emplace_back(uuid, "move");
        for (const char* list : {"added", "removed"}) {
            const auto it = record.find(list);
            if (it == record.end() || !it->is_array()) continue;
            for (const json& id : *it) {
                if (id.is_string()) keys.emplace_back(uuid, "c:" + id.get<std::string>());
            }
        }
        if (const auto components = record.find("components"); components != record.end() && components->is_object()) {
            for (const auto& [component, fields] : components->items()) {
                if (!fields.is_object()) continue;
                for (const auto& [field, value] : fields.items()) keys.emplace_back(uuid, "f:" + component + "." + field);
            }
        }
    }
    if (patch.environment) {
        for (const auto& [setting, value] : patch.environment->items()) keys.emplace_back(kSceneKey, "env:" + setting);
    }
    if (patch.name) keys.emplace_back(kSceneKey, "name");
    return keys;
}

} // namespace

SceneSync::SceneSync(Scene& scene, Applier apply) : scene_(scene), apply_(std::move(apply)) { markSynced(); }

void SceneSync::markSynced() {
    synced_ = SceneIndex::fromScene(scene_);
    pending_.clear();
    inFlight_ = 0;
}

void SceneSync::apply(const ScenePatch& patch) {
    if (patch.empty()) return;
    if (apply_) {
        apply_(patch);
    } else if (auto applied = patch.applyTo(scene_); !applied) {
        log::warn("Collab", "change partly applied: {}", applied.error().message);
    }
}

void SceneSync::replaceWith(const json& document) {
    apply(ScenePatch::between(SceneIndex::fromScene(scene_), SceneIndex::fromDocument(document)));
    markSynced();
}

std::optional<ScenePatch> SceneSync::takeLocalChanges() {
    SceneIndex current = SceneIndex::fromScene(scene_);
    ScenePatch patch = ScenePatch::between(synced_, current);
    if (patch.empty()) return std::nullopt;
    synced_ = std::move(current);
    for (const auto& [entity, key] : keysOf(patch)) ++pending_[entity][key];
    ++inFlight_;
    return patch;
}

bool SceneSync::pending(const std::string& entity, const std::string& key) const {
    const auto it = pending_.find(entity);
    return it != pending_.end() && it->second.contains(key);
}

void SceneSync::receive(const ScenePatch& patch, bool own) {
    if (own) {
        for (const auto& [entity, key] : keysOf(patch)) {
            const auto it = pending_.find(entity);
            if (it == pending_.end()) continue;
            const auto k = it->second.find(key);
            if (k != it->second.end() && --k->second <= 0) it->second.erase(k);
            if (it->second.empty()) pending_.erase(it);
        }
        if (inFlight_ > 0) --inFlight_;
        return;
    }

    // Drop the parts of others' changes that one of our in-flight patches overrides: ours comes
    // later in the host's order, so the host ends up with our value too.
    ScenePatch accepted;
    for (const Uuid& id : patch.removals) {
        if (!pending(id.toString(), "full")) accepted.removals.push_back(id);  // we re-created it
    }
    for (const json& record : patch.upserts) {
        const std::string uuid = record.value("uuid", std::string{});
        if (record.value("full", false)) {
            if (!pending_.contains(uuid)) accepted.upserts.push_back(record);
            continue;
        }
        if (pending(uuid, "full") || pending(uuid, "removed")) continue;
        json kept = {{"uuid", uuid}};
        if (record.contains("name") && !pending(uuid, "name")) kept["name"] = record["name"];
        if (record.contains("move") && !pending(uuid, "move")) kept["move"] = record["move"];
        auto keepIds = [&](const char* list) {
            const auto it = record.find(list);
            if (it == record.end() || !it->is_array()) return;
            json ids = json::array();
            for (const json& id : *it) {
                if (id.is_string() && !pending(uuid, "c:" + id.get<std::string>())) ids.push_back(id);
            }
            if (!ids.empty()) kept[list] = std::move(ids);
        };
        keepIds("added");
        keepIds("removed");
        if (const auto components = record.find("components"); components != record.end() && components->is_object()) {
            json comps = json::object();
            for (const auto& [component, fields] : components->items()) {
                if (!fields.is_object() || pending(uuid, "c:" + component)) continue;
                json keptFields = json::object();
                for (const auto& [field, value] : fields.items()) {
                    if (!pending(uuid, "f:" + component + "." + field)) keptFields[field] = value;
                }
                if (!keptFields.empty() || fields.empty()) comps[component] = std::move(keptFields);
            }
            if (!comps.empty()) kept["components"] = std::move(comps);
        }
        if (kept.size() > 1) accepted.upserts.push_back(std::move(kept));
    }
    if (patch.environment) {
        json settings = json::object();
        for (const auto& [setting, value] : patch.environment->items()) {
            if (!pending(kSceneKey, "env:" + setting)) settings[setting] = value;
        }
        if (!settings.empty()) accepted.environment = std::move(settings);
    }
    if (patch.name && !pending(kSceneKey, "name")) accepted.name = patch.name;
    apply(accepted);
    // Local edits were taken before this call, so the scene now is exactly the exchanged state.
    synced_ = SceneIndex::fromScene(scene_);
}

} // namespace lumora::collab
