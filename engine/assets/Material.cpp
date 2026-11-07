#include "assets/Material.hpp"

#include "assets/MaterialGraph.hpp"
#include "core/FileSystem.hpp"

#include <nlohmann/json.hpp>

#include <system_error>

namespace lumora {

namespace {

nlohmann::json vec(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }

nlohmann::json vec(const glm::vec2& v) { return nlohmann::json::array({v.x, v.y}); }

template <class V>
bool readVec(const nlohmann::json& j, V& out) {
    constexpr int n = V::length();
    if (!j.is_array() || j.size() != static_cast<std::size_t>(n)) return false;
    for (int i = 0; i < n; ++i) {
        if (!j[static_cast<std::size_t>(i)].is_number()) return false;
        out[i] = j[static_cast<std::size_t>(i)].get<float>();
    }
    return true;
}

} // namespace

TextureTransform MaterialAsset::uvTransform() const {
    TextureTransform t;
    t.scale = uvScale;
    t.offset = uvOffset;
    t.rotation = uvRotation * 0.017453292519943295f;
    return t;
}

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
        {"occlusionTexture", m.occlusionTexture},
        {"normalStrength", m.normalStrength},
        {"alphaMode", static_cast<int>(m.alphaMode)},
        {"alphaCutoff", m.alphaCutoff},
        {"opacity", m.opacity},
        {"doubleSided", m.doubleSided},
        {"uvScale", vec(m.uvScale)},
        {"uvOffset", vec(m.uvOffset)},
        {"uvRotation", m.uvRotation},
        {"ior", m.ior},
        {"specularFactor", m.specularFactor},
        {"specularColor", vec(m.specularColor)},
        {"clearcoat", m.clearcoat},
        {"clearcoatRoughness", m.clearcoatRoughness},
        {"sheenColor", vec(m.sheenColor)},
        {"sheenRoughness", m.sheenRoughness},
        {"transmission", m.transmission},
        {"thickness", m.thickness},
        {"attenuationDistance", m.attenuationDistance},
        {"attenuationColor", vec(m.attenuationColor)},
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
    auto pair = [&](const char* key, glm::vec2& out) -> Result<> {
        if (j.contains(key) && !readVec(j[key], out)) return makeError("'{}' must be [x, y]", key);
        return {};
    };
    auto flag = [&](const char* key, bool& out) -> Result<> {
        if (!j.contains(key)) return {};
        if (!j[key].is_boolean()) return makeError("'{}' must be true or false", key);
        out = j[key].get<bool>();
        return {};
    };
    int alphaMode = static_cast<int>(m.alphaMode);
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
                              text("normalTexture", m.normalTexture), text("emissiveTexture", m.emissiveTexture),
                              text("occlusionTexture", m.occlusionTexture), number("normalStrength", m.normalStrength),
                              number("alphaMode", alphaMode), number("alphaCutoff", m.alphaCutoff),
                              number("opacity", m.opacity), flag("doubleSided", m.doubleSided),
                              pair("uvScale", m.uvScale), pair("uvOffset", m.uvOffset), number("uvRotation", m.uvRotation),
                              number("ior", m.ior), number("specularFactor", m.specularFactor),
                              vector("specularColor", m.specularColor), number("clearcoat", m.clearcoat),
                              number("clearcoatRoughness", m.clearcoatRoughness), vector("sheenColor", m.sheenColor),
                              number("sheenRoughness", m.sheenRoughness), number("transmission", m.transmission),
                              number("thickness", m.thickness), number("attenuationDistance", m.attenuationDistance),
                              vector("attenuationColor", m.attenuationColor)}) {
        if (!r) return std::unexpected(r.error());
    }
    if (alphaMode < 0 || alphaMode > 2) return makeError("'alphaMode' must be 0 (opaque), 1 (cut out) or 2 (transparent)");
    m.alphaMode = static_cast<MaterialAsset::AlphaMode>(alphaMode);
    return m;
}

Result<MaterialAsset> loadMaterial(const std::filesystem::path& file) {
    if (file.extension() == kMaterialGraphExtension) {
        auto program = loadMaterialGraph(file);
        if (!program) return std::unexpected(program.error());
        MaterialAsset m;
        m.baseColor = glm::vec3(1.0f);
        m.alphaMode = static_cast<MaterialAsset::AlphaMode>(program->alphaMode);
        m.alphaCutoff = program->alphaCutoff;
        m.doubleSided = program->twoSided;
        m.program = std::make_shared<const MaterialProgram>(std::move(*program));
        return m;
    }
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
