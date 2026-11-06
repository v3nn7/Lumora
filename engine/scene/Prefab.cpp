#include "scene/Prefab.hpp"

#include "core/FileSystem.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace lumora {

using json = nlohmann::json;

namespace {
void collect(const Scene& scene, entt::entity e, json& entities) {
    entities.push_back(SceneSerializer::entityToJson(scene, e));
    for (entt::entity child : scene.childrenOf(e)) collect(scene, child, entities);
}
} // namespace

json prefabToJson(const Scene& scene, entt::entity root) {
    json entities = json::array();
    collect(scene, root, entities);
    entities[0].erase("parent");  // the root attaches wherever it is instantiated
    return {{"format", "lumora.prefab"}, {"version", 1}, {"entities", std::move(entities)}};
}

Result<> savePrefab(const Scene& scene, entt::entity root, const std::filesystem::path& file) {
    if (!scene.registry().valid(root)) return makeError("no entity to save as a prefab");
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return fs::writeTextFileAtomic(file, prefabToJson(scene, root).dump(2) + "\n");
}

Result<json> loadPrefab(const std::filesystem::path& file) {
    auto text = fs::readTextFile(file);
    if (!text) return std::unexpected(text.error());
    json prefab = json::parse(*text, nullptr, false);
    if (prefab.is_discarded() || prefab.value("format", "") != "lumora.prefab" || !prefab.contains("entities") ||
        !prefab["entities"].is_array() || prefab["entities"].empty()) {
        return makeError("{} is not a Lumora prefab", file.filename().string());
    }
    return prefab;
}

Result<entt::entity> instantiatePrefab(Scene& scene, const json& prefab, entt::entity parent) {
    const json& entities = prefab.at("entities");
    std::unordered_map<std::string, entt::entity> created;  // prefab uuid -> new entity
    entt::entity root = entt::null;
    for (const json& record : entities) {
        if (!record.is_object()) return makeError("malformed prefab entity");
        entt::entity attachTo = parent;
        if (const auto p = record.find("parent"); p != record.end() && p->is_string()) {
            const auto found = created.find(p->get<std::string>());
            if (found == created.end()) return makeError("prefab entity refers to a parent defined after it");
            attachTo = found->second;
        } else if (root != entt::null) {
            return makeError("a prefab has exactly one root");
        }
        const entt::entity e = scene.createEntity(record.value("name", std::string("Entity")), Uuid::generate(), attachTo);
        if (const auto c = record.find("components"); c != record.end()) {
            SceneSerializer::applyComponents(scene, e, *c, false);
        }
        if (root == entt::null) root = e;
        if (const auto u = record.find("uuid"); u != record.end() && u->is_string()) created[u->get<std::string>()] = e;
    }
    scene.updateWorldTransforms();
    return root;
}

bool isPrefabFile(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".lprefab";
}

} // namespace lumora
