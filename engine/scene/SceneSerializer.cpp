#include "scene/SceneSerializer.hpp"

#include "core/FileSystem.hpp"
#include "core/Log.hpp"
#include "scene/ComponentRegistry.hpp"

#include <nlohmann/json.hpp>

namespace lumora {

using nlohmann::json;

namespace {

json vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

std::optional<glm::vec3> vec3FromJson(const json& j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    for (const auto& c : j) {
        if (!c.is_number()) return std::nullopt;
    }
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

json fieldToJson(const FieldValue& value) {
    return std::visit(
        [](const auto& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, glm::vec3>) {
                return vec3ToJson(v);
            } else {
                return json(v);
            }
        },
        value);
}

std::optional<FieldValue> fieldFromJson(FieldKind kind, const json& j) {
    switch (kind) {
        case FieldKind::Bool:
            if (j.is_boolean()) return FieldValue(j.get<bool>());
            break;
        case FieldKind::Int:
        case FieldKind::Enum:
            if (j.is_number_integer()) return FieldValue(j.get<int>());
            break;
        case FieldKind::Float:
            if (j.is_number()) return FieldValue(j.get<float>());
            break;
        case FieldKind::Vec3:
        case FieldKind::Color:
            if (auto v = vec3FromJson(j)) return FieldValue(*v);
            break;
        case FieldKind::String:
        case FieldKind::Asset:
            if (j.is_string()) return FieldValue(j.get<std::string>());
            break;
    }
    return std::nullopt;
}

} // namespace

json SceneSerializer::entityToJson(const Scene& scene, entt::entity e) {
    const auto& reg = scene.registry();
    json out;
    out["uuid"] = scene.uuidOf(e).toString();
    out["name"] = scene.nameOf(e);
    const entt::entity parent = scene.parentOf(e);
    if (parent != entt::null) {
        out["parent"] = scene.uuidOf(parent).toString();
    }
    json components = json::object();
    for (const ComponentDesc& desc : ComponentRegistry::builtin().components()) {
        if (!desc.has(reg, e)) continue;
        json fields = json::object();
        for (const FieldDesc& f : desc.fields) {
            fields[f.id] = fieldToJson(f.get(reg, e));
        }
        components[desc.id] = std::move(fields);
    }
    out["components"] = std::move(components);
    return out;
}

json SceneSerializer::environmentToJson(const EnvironmentSettings& env) {
    return {
        {"skyZenithColor", vec3ToJson(env.skyZenithColor)},
        {"skyHorizonColor", vec3ToJson(env.skyHorizonColor)},
        {"groundColor", vec3ToJson(env.groundColor)},
        {"ambientIntensity", env.ambientIntensity},
        {"exposure", env.exposure},
        {"fogDensity", env.fogDensity},
        {"fogHeightFalloff", env.fogHeightFalloff},
        {"bloomIntensity", env.bloomIntensity},
        {"vignette", env.vignette},
        {"contrast", env.contrast},
        {"saturation", env.saturation},
        {"temperature", env.temperature},
        {"tint", env.tint},
        {"dofBlur", env.dofBlur},
        {"focusDistance", env.focusDistance},
        {"focusRange", env.focusRange},
        {"sunShafts", env.sunShafts},
        {"virtualShadowMaps", env.virtualShadowMaps},
        {"skyTexture", env.skyTexture},
        {"skyIntensity", env.skyIntensity},
        {"skyRotation", env.skyRotationDeg},
    };
}

void SceneSerializer::applyEnvironment(EnvironmentSettings& env, const json& j) {
    if (!j.is_object()) return;
    auto number = [&](const char* key, float& target) {
        if (const auto it = j.find(key); it != j.end() && it->is_number()) target = it->get<float>();
    };
    auto colour = [&](const char* key, glm::vec3& target) {
        if (const auto it = j.find(key); it != j.end()) {
            if (auto v = vec3FromJson(*it)) target = *v;
        }
    };
    colour("skyZenithColor", env.skyZenithColor);
    colour("skyHorizonColor", env.skyHorizonColor);
    colour("groundColor", env.groundColor);
    number("ambientIntensity", env.ambientIntensity);
    number("exposure", env.exposure);
    number("fogDensity", env.fogDensity);
    number("fogHeightFalloff", env.fogHeightFalloff);
    number("bloomIntensity", env.bloomIntensity);
    number("vignette", env.vignette);
    number("contrast", env.contrast);
    number("saturation", env.saturation);
    number("temperature", env.temperature);
    number("tint", env.tint);
    number("dofBlur", env.dofBlur);
    number("focusDistance", env.focusDistance);
    number("focusRange", env.focusRange);
    number("sunShafts", env.sunShafts);
    if (const auto it = j.find("virtualShadowMaps"); it != j.end() && it->is_boolean()) env.virtualShadowMaps = it->get<bool>();
    if (const auto it = j.find("skyTexture"); it != j.end() && it->is_string()) env.skyTexture = it->get<std::string>();
    number("skyIntensity", env.skyIntensity);
    number("skyRotation", env.skyRotationDeg);
}

void SceneSerializer::applyComponents(Scene& scene, entt::entity e, const json& componentsJson, bool removeMissing) {
    auto& reg = scene.registry();
    const ComponentRegistry& components = ComponentRegistry::builtin();
    const std::string label = scene.uuidOf(e).toString();
    if (componentsJson.is_object()) {
        for (const auto& [componentId, fields] : componentsJson.items()) {
            const ComponentDesc* desc = components.find(componentId);
            if (desc == nullptr) {
                log::warn("Scene", "skipping unknown component '{}' on entity {}", componentId, label);
                continue;
            }
            if (!desc->has(reg, e)) desc->add(reg, e);
            if (!fields.is_object()) continue;
            for (const auto& [fieldId, value] : fields.items()) {
                const FieldDesc* f = desc->findField(fieldId);
                if (f == nullptr) continue;
                if (auto parsed = fieldFromJson(f->kind, value)) {
                    f->set(reg, e, *parsed);
                } else {
                    log::warn("Scene", "invalid value for {}.{} on entity {}", componentId, fieldId, label);
                }
            }
        }
    }
    if (!removeMissing) return;
    for (const ComponentDesc& desc : components.components()) {
        if (!desc.removable || !desc.has(reg, e)) continue;
        if (componentsJson.is_object() && componentsJson.contains(desc.id)) continue;
        desc.remove(reg, e);
    }
}

json SceneSerializer::toJson(const Scene& scene) {
    json root;
    root["format"] = "lumora.scene";
    root["version"] = kFormatVersion;
    root["name"] = scene.name();
    root["environment"] = environmentToJson(scene.environment());
    json entities = json::array();
    // Depth-first order guarantees parents precede children and preserves sibling order.
    scene.forEachDepthFirst([&](entt::entity e, int) { entities.push_back(entityToJson(scene, e)); });
    root["entities"] = std::move(entities);
    return root;
}

Result<std::unique_ptr<Scene>> SceneSerializer::fromJson(const json& root) {
    auto scene = std::make_unique<Scene>();
    if (auto loaded = loadInto(*scene, root); !loaded) {
        return std::unexpected(loaded.error());
    }
    return scene;
}

namespace {

Result<> populateScene(Scene& target, const json& root) {
    if (!root.is_object() || root.value("format", "") != "lumora.scene") {
        return makeError("not a Lumora scene file");
    }
    const int version = root.value("version", 0);
    if (version < 1 || version > SceneSerializer::kFormatVersion) {
        return makeError("unsupported scene format version {} (supported: 1..{})", version, SceneSerializer::kFormatVersion);
    }
    target.clear();
    target.setName(root.value("name", std::string("Untitled")));
    Scene* scene = &target;

    if (const auto it = root.find("environment"); it != root.end()) {
        SceneSerializer::applyEnvironment(scene->environment(), *it);
    }

    const auto entitiesIt = root.find("entities");
    if (entitiesIt == root.end() || !entitiesIt->is_array()) {
        return makeError("scene has no 'entities' array");
    }
    for (const json& ej : *entitiesIt) {
        if (!ej.is_object()) {
            return makeError("entity entry is not an object");
        }
        auto uuid = Uuid::parse(ej.value("uuid", std::string{}));
        if (!uuid) {
            return makeError("entity '{}': {}", ej.value("name", std::string{}), uuid.error().message);
        }
        entt::entity parent = entt::null;
        if (const auto p = ej.find("parent"); p != ej.end() && p->is_string()) {
            auto parentId = Uuid::parse(p->get<std::string>());
            if (!parentId) return std::unexpected(parentId.error());
            parent = scene->findByUuid(*parentId);
            if (parent == entt::null) {
                return makeError("entity {} references unknown parent {}", uuid->toString(), parentId->toString());
            }
        }
        if (scene->findByUuid(*uuid) != entt::null) {
            return makeError("duplicate entity uuid {}", uuid->toString());
        }
        const entt::entity e = scene->createEntity(ej.value("name", std::string("Entity")), *uuid, parent);
        if (const auto compsIt = ej.find("components"); compsIt != ej.end()) {
            SceneSerializer::applyComponents(*scene, e, *compsIt, false);
        }
    }
    scene->updateWorldTransforms();
    return {};
}

} // namespace

Result<> SceneSerializer::loadInto(Scene& scene, const json& document) {
    auto result = populateScene(scene, document);
    if (!result) scene.clear();  // never leave a half-loaded scene behind
    return result;
}

std::string SceneSerializer::toString(const Scene& scene) { return toJson(scene).dump(2); }

Result<std::unique_ptr<Scene>> SceneSerializer::fromString(const std::string& text) {
    json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        return makeError("scene file is not valid JSON");
    }
    return fromJson(root);
}

Result<> SceneSerializer::save(const Scene& scene, const std::filesystem::path& path) {
    return fs::writeTextFileAtomic(path, toString(scene) + "\n");
}

Result<std::unique_ptr<Scene>> SceneSerializer::load(const std::filesystem::path& path) {
    auto text = fs::readTextFile(path);
    if (!text) return std::unexpected(text.error());
    auto scene = fromString(*text);
    if (!scene) {
        return makeError("{}: {}", path.string(), scene.error().message);
    }
    return scene;
}

Result<std::unique_ptr<Scene>> SceneSerializer::clone(const Scene& scene) { return fromJson(toJson(scene)); }

} // namespace lumora
