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
            if (j.is_string()) return FieldValue(j.get<std::string>());
            break;
    }
    return std::nullopt;
}

json entityToJson(const Scene& scene, entt::entity e) {
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

} // namespace

json SceneSerializer::toJson(const Scene& scene) {
    json root;
    root["format"] = "lumora.scene";
    root["version"] = kFormatVersion;
    root["name"] = scene.name();
    const EnvironmentSettings& env = scene.environment();
    root["environment"] = {
        {"skyZenithColor", vec3ToJson(env.skyZenithColor)},
        {"skyHorizonColor", vec3ToJson(env.skyHorizonColor)},
        {"groundColor", vec3ToJson(env.groundColor)},
        {"ambientIntensity", env.ambientIntensity},
        {"exposure", env.exposure},
    };
    json entities = json::array();
    // Depth-first order guarantees parents precede children and preserves sibling order.
    scene.forEachDepthFirst([&](entt::entity e, int) { entities.push_back(entityToJson(scene, e)); });
    root["entities"] = std::move(entities);
    return root;
}

Result<std::unique_ptr<Scene>> SceneSerializer::fromJson(const json& root) {
    if (!root.is_object() || root.value("format", "") != "lumora.scene") {
        return makeError("not a Lumora scene file");
    }
    const int version = root.value("version", 0);
    if (version < 1 || version > kFormatVersion) {
        return makeError("unsupported scene format version {} (supported: 1..{})", version, kFormatVersion);
    }
    auto scene = std::make_unique<Scene>(root.value("name", std::string("Untitled")));

    if (const auto it = root.find("environment"); it != root.end() && it->is_object()) {
        EnvironmentSettings& env = scene->environment();
        const json& j = *it;
        if (auto v = j.contains("skyZenithColor") ? vec3FromJson(j["skyZenithColor"]) : std::nullopt) env.skyZenithColor = *v;
        if (auto v = j.contains("skyHorizonColor") ? vec3FromJson(j["skyHorizonColor"]) : std::nullopt) env.skyHorizonColor = *v;
        if (auto v = j.contains("groundColor") ? vec3FromJson(j["groundColor"]) : std::nullopt) env.groundColor = *v;
        env.ambientIntensity = j.value("ambientIntensity", env.ambientIntensity);
        env.exposure = j.value("exposure", env.exposure);
    }

    const auto entitiesIt = root.find("entities");
    if (entitiesIt == root.end() || !entitiesIt->is_array()) {
        return makeError("scene has no 'entities' array");
    }
    auto& reg = scene->registry();
    const ComponentRegistry& components = ComponentRegistry::builtin();
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
        const entt::entity e = scene->createEntity(ej.value("name", std::string("Entity")), *uuid, parent);
        const auto compsIt = ej.find("components");
        if (compsIt == ej.end() || !compsIt->is_object()) continue;
        for (const auto& [componentId, fields] : compsIt->items()) {
            const ComponentDesc* desc = components.find(componentId);
            if (desc == nullptr) {
                log::warn("Scene", "skipping unknown component '{}' on entity {}", componentId, uuid->toString());
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
                    log::warn("Scene", "invalid value for {}.{} on entity {}", componentId, fieldId, uuid->toString());
                }
            }
        }
    }
    scene->updateWorldTransforms();
    return scene;
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
