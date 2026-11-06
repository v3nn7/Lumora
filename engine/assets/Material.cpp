#include "assets/Material.hpp"

#include "core/FileSystem.hpp"

#include <nlohmann/json.hpp>

#include <system_error>

namespace lumora {

namespace {

nlohmann::json vec(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }

bool readVec(const nlohmann::json& j, glm::vec3& out) {
    if (!j.is_array() || j.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!j[static_cast<std::size_t>(i)].is_number()) return false;
        out[i] = j[static_cast<std::size_t>(i)].get<float>();
    }
    return true;
}

} // namespace

nlohmann::json materialToJson(const MaterialAsset& m) {
    return {
        {"format", 1},
        {"baseColor", vec(m.baseColor)},
        {"metallic", m.metallic},
        {"roughness", m.roughness},
        {"emissive", vec(m.emissive)},
        {"emissiveStrength", m.emissiveStrength},
        {"pattern", m.pattern},
        {"baseColorTexture", m.baseColorTexture},
        {"metallicRoughnessTexture", m.metallicRoughnessTexture},
        {"normalTexture", m.normalTexture},
        {"emissiveTexture", m.emissiveTexture},
    };
}

Result<MaterialAsset> materialFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return makeError("a material must be a JSON object");
    MaterialAsset m;
    auto number = [&](const char* key, auto& out) -> Result<> {
        if (!j.contains(key)) return {};
        if (!j[key].is_number()) return makeError("'{}' must be a number", key);
        out = j[key].get<std::remove_reference_t<decltype(out)>>();
        return {};
    };
    auto vector = [&](const char* key, glm::vec3& out) -> Result<> {
        if (j.contains(key) && !readVec(j[key], out)) return makeError("'{}' must be [x, y, z]", key);
        return {};
    };
    auto text = [&](const char* key, std::string& out) -> Result<> {
        if (!j.contains(key)) return {};
        if (!j[key].is_string()) return makeError("'{}' must be a string", key);
        out = j[key].get<std::string>();
        return {};
    };
    for (const Result<>& r : {vector("baseColor", m.baseColor), number("metallic", m.metallic),
                              number("roughness", m.roughness), vector("emissive", m.emissive),
                              number("emissiveStrength", m.emissiveStrength), number("pattern", m.pattern),
                              text("baseColorTexture", m.baseColorTexture),
                              text("metallicRoughnessTexture", m.metallicRoughnessTexture),
                              text("normalTexture", m.normalTexture), text("emissiveTexture", m.emissiveTexture)}) {
        if (!r) return std::unexpected(r.error());
    }
    return m;
}

Result<MaterialAsset> loadMaterial(const std::filesystem::path& file) {
    auto text = fs::readTextFile(file);
    if (!text) return std::unexpected(text.error());
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded()) return makeError("{}: not valid JSON", file.string());
    auto m = materialFromJson(j);
    if (!m) return makeError("{}: {}", file.string(), m.error().message);
    return m;
}

Result<> saveMaterial(const MaterialAsset& material, const std::filesystem::path& file) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    return fs::writeTextFileAtomic(file, materialToJson(material).dump(2) + "\n");
}

} // namespace lumora
