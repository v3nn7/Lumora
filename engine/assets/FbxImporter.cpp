#include "assets/FbxImporter.hpp"

#include "core/FileSystem.hpp"

#include <ufbx.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lumora {

namespace {

struct SceneDeleter {
    void operator()(ufbx_scene* scene) const { ufbx_free_scene(scene); }
};
using ScenePtr = std::unique_ptr<ufbx_scene, SceneDeleter>;

struct BakeDeleter {
    void operator()(ufbx_baked_anim* bake) const { ufbx_free_baked_anim(bake); }
};

std::string text(ufbx_string s) { return std::string(s.data, s.length); }

std::filesystem::path utf8Path(std::string s) {
#if !defined(_WIN32)
    std::ranges::replace(s, '\\', '/');  // files made on Windows
#endif
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string u = path.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

glm::vec3 toVec3(ufbx_vec3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
glm::quat toQuat(ufbx_quat q) {
    return glm::quat(static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z));
}
glm::mat4 toMat4(const ufbx_matrix& m) {
    glm::mat4 out(1.0f);
    for (int c = 0; c < 4; ++c) out[c] = glm::vec4(toVec3(m.cols[c]), c == 3 ? 1.0f : 0.0f);
    return out;
}

bool isIdentity(const ufbx_transform& t) {
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    return close(t.translation.x, 0) && close(t.translation.y, 0) && close(t.translation.z, 0) && close(t.rotation.x, 0) &&
           close(t.rotation.y, 0) && close(t.rotation.z, 0) && close(std::abs(t.rotation.w), 1) && close(t.scale.x, 1) &&
           close(t.scale.y, 1) && close(t.scale.z, 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Result<ScenePtr> load(const std::filesystem::path& file, bool contentOnly) {
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;  // units stay the file's: ImportedModel::unitScale
    opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    opts.generate_missing_normals = true;
    opts.clean_skin_weights = true;
    opts.load_external_files = true;  // an OBJ's material library
    opts.ignore_missing_external_files = true;
    opts.obj_search_mtl_by_filename = true;
    opts.obj_unit_meters = 1.0;
    opts.obj_axes = ufbx_axes_right_handed_y_up;
    opts.node_depth_limit = 512;
    if (contentOnly) {
        opts.ignore_geometry = true;
        opts.ignore_animation = true;
        opts.ignore_embedded = true;
    }
    const std::string name = utf8(file);
    ufbx_error error{};
    ufbx_scene* scene = ufbx_load_file_len(name.data(), name.size(), &opts, &error);
    if (scene == nullptr) {
        char message[1024];
        const std::size_t length = ufbx_format_error(message, sizeof message, &error);
        std::string what(message, std::min(length, sizeof message - 1));
        while (!what.empty() && (what.back() == '\n' || what.back() == ' ')) what.pop_back();
        return makeError("{}: {}", file.filename().string(), what);
    }
    return ScenePtr(scene);
}

const ufbx_texture* fileTexture(const ufbx_texture* texture) {
    if (texture == nullptr) return nullptr;
    if (texture->type == UFBX_TEXTURE_FILE) return texture;
    return texture->file_textures.count > 0 ? texture->file_textures.data[0] : nullptr;
}

/// Where a texture's file is: as written (relative to the model, or absolute), or, as files
/// usually travel without the author's folders, by its name next to the model.
std::optional<std::filesystem::path> findTextureFile(const std::filesystem::path& dir, const ufbx_texture& texture) {
    std::vector<std::filesystem::path> candidates;
    std::vector<std::string> names;
    for (const ufbx_string s : {texture.relative_filename, texture.filename, texture.absolute_filename}) {
        if (s.length == 0) continue;
        const std::filesystem::path p = utf8Path(text(s));
        candidates.push_back(p.is_absolute() ? p : dir / p);
        names.push_back(utf8(p.filename()));
    }
    for (const std::string& name : names) {
        if (name.empty()) continue;
        for (const char* sub : {"", "textures", "Textures", "texture", "maps", "images"}) {
            candidates.push_back(dir / sub / utf8Path(name));
        }
    }
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate.lexically_normal();
    }
    return std::nullopt;
}

std::string mimeTypeOf(const std::filesystem::path& file) {
    const std::string ext = lower(file.extension().string());
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".ktx2") return "image/ktx2";
    return "";
}

class Importer {
public:
    Importer(const ufbx_scene& scene, std::filesystem::path dir) : scene_(scene), dir_(std::move(dir)) {}

    ImportedModel run() {
        const ufbx_metadata& meta = scene_.metadata;
        materials();
        meshes();
        nodes();
        skins();
        animations();
        const double unit = scene_.settings.unit_meters;
        if (unit > 0.0 && std::abs(unit - 1.0) > 1e-6) {
            model_.unitScale = static_cast<float>(unit);
            model_.warnings.push_back(std::format("units of {} m, scaled to metres (x {})", unit, unit));
        }
        const std::string application = text(meta.original_application.name) + " " + text(meta.latest_application.name);
        if (application.find("SketchUp") != std::string::npos) {
            for (ImportedMaterial& m : model_.materials) m.doubleSided = true;  // SketchUp shows both sides
        }
        if (meta.warnings.count > 0) {
            model_.warnings.push_back(std::format("{} problem(s) in the file (first: {})", meta.warnings.count,
                                                  text(meta.warnings.data[0].description)));
        }
        finalizeImportedModel(model_);
        return std::move(model_);
    }

private:
    int image(const ufbx_texture* source, const ImportedMaterial& material, const char* use) {
        const ufbx_texture* texture = fileTexture(source);
        if (texture == nullptr) return -1;
        if (const auto it = byTexture_.find(texture); it != byTexture_.end()) return it->second;
        ImportedImage out;
        out.name = texture->name.length > 0 ? text(texture->name) : std::format("{} {}", material.name, use);
        std::string key;
        if (texture->content.size > 0) {
            const auto* bytes = static_cast<const std::byte*>(texture->content.data);
            out.encoded.assign(bytes, bytes + texture->content.size);
            out.mimeType = mimeTypeOf(utf8Path(text(texture->filename)));
            key = std::format("embedded:{}", texture->element_id);
        } else if (const auto file = findTextureFile(dir_, *texture)) {
            key = utf8(*file);
            if (const auto it = byFile_.find(key); it != byFile_.end()) return byTexture_[texture] = it->second;
            auto bytes = fs::readBinaryFile(*file);
            if (!bytes) {
                model_.warnings.push_back(std::format("texture '{}': {}", out.name, bytes.error().message));
                return byTexture_[texture] = -1;
            }
            out.encoded = std::move(*bytes);
            out.mimeType = mimeTypeOf(*file);
        } else {
            model_.warnings.push_back(std::format("material '{}': {} map '{}' not found next to the model", material.name, use,
                                                  utf8(utf8Path(text(texture->filename)).filename())));
            return byTexture_[texture] = -1;
        }
        const int index = static_cast<int>(model_.images.size());
        model_.images.push_back(std::move(out));
        byFile_[key] = index;
        return byTexture_[texture] = index;
    }

    void materials() {
        for (std::size_t i = 0; i < scene_.materials.count; ++i) {
            const ufbx_material& src = *scene_.materials.data[i];
            const ufbx_material_pbr_maps& pbr = src.pbr;
            ImportedMaterial mat;
            mat.name = src.name.length > 0 ? text(src.name) : std::format("Material {}", i);
            mat.baseColorTexture = image(pbr.base_color.texture, mat, "base colour");
            glm::vec3 color(1.0f);
            if (mat.baseColorTexture < 0 && pbr.base_color.has_value) {
                // FBX: a connected texture replaces the colour (glTF would multiply them).
                color = toVec3(pbr.base_color.value_vec3);
                if (pbr.base_factor.has_value) color *= static_cast<float>(pbr.base_factor.value_real);
            }
            const float opacity = pbr.opacity.has_value ? std::clamp(static_cast<float>(pbr.opacity.value_real), 0.0f, 1.0f) : 1.0f;
            mat.baseColor = glm::vec4(glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f)), opacity);
            if (opacity < 0.999f) mat.alphaMode = ImportedMaterial::AlphaMode::Blend;
            mat.metallic = pbr.metalness.has_value ? std::clamp(static_cast<float>(pbr.metalness.value_real), 0.0f, 1.0f) : 0.0f;
            mat.roughness = pbr.roughness.has_value ? std::clamp(static_cast<float>(pbr.roughness.value_real), 0.0f, 1.0f) : 0.8f;
            const ufbx_texture* rough = fileTexture(pbr.roughness.texture);
            const ufbx_texture* metal = fileTexture(pbr.metalness.texture);
            if (rough != nullptr && rough == metal) {
                // One map for both: assume glTF's packing (roughness in G, metalness in B).
                mat.metallicRoughnessTexture = image(rough, mat, "metallic-roughness");
                mat.metallic = 1.0f;
                mat.roughness = 1.0f;
            } else if (rough != nullptr || metal != nullptr) {
                model_.warnings.push_back(std::format("material '{}': separate roughness/metalness maps are not supported", mat.name));
            }
            mat.normalTexture = image(pbr.normal_map.texture, mat, "normal");
            mat.occlusionTexture = image(pbr.ambient_occlusion.texture, mat, "occlusion");
            mat.emissiveTexture = image(pbr.emission_color.texture, mat, "emission");
            glm::vec3 emissive = pbr.emission_color.has_value ? toVec3(pbr.emission_color.value_vec3) : glm::vec3(0.0f);
            if (mat.emissiveTexture >= 0 && glm::all(glm::lessThan(emissive, glm::vec3(1e-4f)))) emissive = glm::vec3(1.0f);
            const float emissionFactor = pbr.emission_factor.has_value ? static_cast<float>(pbr.emission_factor.value_real) : 1.0f;
            mat.emissive = glm::max(emissive * emissionFactor, glm::vec3(0.0f));
            model_.materials.push_back(std::move(mat));
        }
    }

    void meshes() {
        // Like glTF: mesh m (of those with triangles) keeps index m for its first material, the
        // other materials are appended after all of them.
        std::vector<ImportedMesh> extra;
        meshGroup_.assign(scene_.meshes.count, -1);
        for (std::size_t m = 0; m < scene_.meshes.count; ++m) {
            const ufbx_mesh& src = *scene_.meshes.data[m];
            if (src.num_triangles == 0 || !src.vertex_position.exists) continue;
            const std::string baseName = src.name.length > 0 ? text(src.name)
                                         : src.instances.count > 0 && src.instances.data[0]->name.length > 0
                                             ? text(src.instances.data[0]->name)
                                             : std::format("Mesh {}", m);
            std::vector<int> parts;
            for (std::size_t p = 0; p < src.material_parts.count; ++p) {
                const ufbx_mesh_part& part = src.material_parts.data[p];
                if (part.num_triangles == 0) continue;
                ImportedMesh mesh;
                mesh.name = parts.empty() ? baseName : std::format("{} ({})", baseName, parts.size());
                mesh.material = part.index < src.materials.count ? static_cast<int>(src.materials.data[part.index]->typed_id) : -1;
                fill(src, part, mesh.data);
                if (parts.empty()) {
                    parts.push_back(static_cast<int>(model_.meshes.size()));
                    model_.meshes.push_back(std::move(mesh));
                } else {
                    parts.push_back(-1 - static_cast<int>(extra.size()));
                    extra.push_back(std::move(mesh));
                }
            }
            if (parts.empty()) continue;
            meshGroup_[m] = static_cast<int>(model_.meshParts.size());
            model_.meshParts.push_back(std::move(parts));
        }
        const int first = static_cast<int>(model_.meshes.size());
        for (auto& parts : model_.meshParts) {
            for (int& part : parts) {
                if (part < 0) part = first + (-1 - part);
            }
        }
        for (ImportedMesh& mesh : extra) model_.meshes.push_back(std::move(mesh));
    }

    struct CornerKey {
        std::uint32_t vertex, normal, uv;
        bool operator==(const CornerKey&) const = default;
    };
    struct CornerHash {
        std::size_t operator()(const CornerKey& k) const noexcept {
            return (static_cast<std::size_t>(k.vertex) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::size_t>(k.normal) * 0xC2B2AE3D27D4EB4Full) ^
                   (static_cast<std::size_t>(k.uv) * 0x165667B19E3779F9ull);
        }
    };

    static void fill(const ufbx_mesh& src, const ufbx_mesh_part& part, MeshData& out) {
        const ufbx_skin_deformer* skin = src.skin_deformers.count > 0 ? src.skin_deformers.data[0] : nullptr;
        std::vector<std::uint32_t> corners(src.max_face_triangles * 3);
        std::unordered_map<CornerKey, std::uint32_t, CornerHash> unique;
        for (std::size_t f = 0; f < part.face_indices.count; ++f) {
            const ufbx_face face = src.faces.data[part.face_indices.data[f]];
            const std::uint32_t triangles = ufbx_triangulate_face(corners.data(), corners.size(), &src, face);
            for (std::uint32_t c = 0; c < triangles * 3; ++c) {
                const std::uint32_t index = corners[c];
                const CornerKey key{src.vertex_indices.data[index],
                                    src.vertex_normal.exists ? src.vertex_normal.indices.data[index] : 0u,
                                    src.vertex_uv.exists ? src.vertex_uv.indices.data[index] : 0u};
                const auto [it, added] = unique.try_emplace(key, static_cast<std::uint32_t>(out.vertices.size()));
                if (added) {
                    Vertex v;
                    v.position = toVec3(ufbx_get_vertex_vec3(&src.vertex_position, index));
                    if (src.vertex_normal.exists) {
                        const glm::vec3 n = toVec3(ufbx_get_vertex_vec3(&src.vertex_normal, index));
                        const float length = glm::length(n);
                        v.normal = length > 1e-12f ? n / length : glm::vec3(0.0f, 1.0f, 0.0f);
                    }
                    if (src.vertex_uv.exists) {
                        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&src.vertex_uv, index);
                        v.uv = glm::vec2(static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y));  // FBX/OBJ: v up
                    }
                    out.vertices.push_back(v);
                    if (skin != nullptr) out.skin.push_back(skinVertex(*skin, key.vertex));
                }
                out.indices.push_back(it->second);
            }
        }
        generateTangents(out);
        out.bounds = computeBounds(out.vertices);
    }

    static SkinVertex skinVertex(const ufbx_skin_deformer& skin, std::uint32_t vertex) {
        SkinVertex out;
        if (vertex >= skin.vertices.count) return out;
        const ufbx_skin_vertex& influences = skin.vertices.data[vertex];
        float total = 0.0f;
        const std::uint32_t count = std::min<std::uint32_t>(influences.num_weights, 4);  // the largest four
        for (std::uint32_t j = 0; j < count; ++j) {
            const ufbx_skin_weight& w = skin.weights.data[influences.weight_begin + j];
            out.joints[j] = static_cast<std::uint16_t>(std::min<std::uint32_t>(w.cluster_index, UINT16_MAX));
            out.weights[static_cast<int>(j)] = static_cast<float>(w.weight);
            total += static_cast<float>(w.weight);
        }
        if (total > 1e-6f) {
            out.weights /= total;
        } else {
            out = SkinVertex{};
        }
        return out;
    }

    void nodes() {
        const ufbx_node* root = scene_.root_node;
        if (root == nullptr) return;
        // The root carries the axis conversion (Z-up files); an identity root is left out.
        if (isIdentity(root->local_transform) && root->mesh == nullptr) {
            for (std::size_t c = 0; c < root->children.count; ++c) visit(*root->children.data[c], -1);
        } else {
            visit(*root, -1);
        }
    }

    void visit(const ufbx_node& node, int parent) {
        ImportedNode out;
        out.name = node.name.length > 0 ? text(node.name)
                   : node.is_root            ? std::string("Root")
                   : node.mesh != nullptr && node.mesh->name.length > 0 ? text(node.mesh->name)
                                                                        : std::string("Node");
        out.parent = parent;
        out.translation = toVec3(node.local_transform.translation);
        out.rotation = toQuat(node.local_transform.rotation);
        out.scale = toVec3(node.local_transform.scale);
        if (node.mesh != nullptr && node.mesh->typed_id < meshGroup_.size()) {
            out.mesh = meshGroup_[node.mesh->typed_id];
            if (out.mesh >= 0 && node.mesh->skin_deformers.count > 0) {
                out.skin = static_cast<int>(node.mesh->skin_deformers.data[0]->typed_id);
            }
        }
        const int index = static_cast<int>(model_.nodes.size());
        nodeIndex_[&node] = index;
        model_.nodes.push_back(std::move(out));
        for (std::size_t c = 0; c < node.children.count; ++c) visit(*node.children.data[c], index);
    }

    void skins() {
        for (std::size_t s = 0; s < scene_.skin_deformers.count; ++s) {
            const ufbx_skin_deformer& src = *scene_.skin_deformers.data[s];
            ImportedSkin skin;
            skin.name = src.name.length > 0 ? text(src.name) : std::format("Skin {}", s);
            bool complete = true;
            for (std::size_t c = 0; c < src.clusters.count; ++c) {
                const ufbx_skin_cluster& cluster = *src.clusters.data[c];
                const auto found = cluster.bone_node != nullptr ? nodeIndex_.find(cluster.bone_node) : nodeIndex_.end();
                if (found == nodeIndex_.end()) complete = false;
                skin.joints.push_back(found != nodeIndex_.end() ? found->second : -1);
                // Mesh geometry to the bone at bind time: glTF's inverse bind matrix.
                skin.inverseBind.push_back(toMat4(cluster.geometry_to_bone));
            }
            if (!complete) model_.warnings.push_back(std::format("skin '{}': bones outside the scene", skin.name));
            model_.skins.push_back(std::move(skin));
        }
    }

    void animations() {
        for (std::size_t a = 0; a < scene_.anim_stacks.count; ++a) {
            const ufbx_anim_stack& stack = *scene_.anim_stacks.data[a];
            ufbx_bake_opts opts{};
            opts.trim_start_time = true;
            opts.resample_rate = 30.0;
            opts.key_reduction_enabled = true;
            ufbx_error error{};
            std::unique_ptr<ufbx_baked_anim, BakeDeleter> baked(ufbx_bake_anim(&scene_, stack.anim, &opts, &error));
            const std::string name = stack.name.length > 0 ? text(stack.name) : std::format("Animation {}", a);
            if (!baked) {
                model_.warnings.push_back(std::format("animation '{}' could not be baked", name));
                continue;
            }
            ImportedAnimation animation;
            animation.name = name;
            for (std::size_t b = 0; b < baked->nodes.count; ++b) {
                const ufbx_baked_node& bn = baked->nodes.data[b];
                if (bn.typed_id >= scene_.nodes.count) continue;
                const auto found = nodeIndex_.find(scene_.nodes.data[bn.typed_id]);
                if (found == nodeIndex_.end()) continue;
                const ImportedNode& rest = model_.nodes[static_cast<std::size_t>(found->second)];
                addChannel(animation, found->second, AnimationChannel::Path::Translation, bn.translation_keys, bn.constant_translation,
                           glm::vec4(rest.translation, 0.0f));
                addChannel(animation, found->second, AnimationChannel::Path::Scale, bn.scale_keys, bn.constant_scale,
                           glm::vec4(rest.scale, 0.0f));
                if (bn.rotation_keys.count == 0) continue;
                const bool same = bn.constant_rotation &&
                                  std::abs(glm::dot(toQuat(bn.rotation_keys.data[0].value), rest.rotation)) > 0.99999f;
                if (same) continue;
                AnimationChannel channel;
                channel.node = found->second;
                channel.path = AnimationChannel::Path::Rotation;
                for (std::size_t k = 0; k < bn.rotation_keys.count; ++k) {
                    const ufbx_baked_quat& key = bn.rotation_keys.data[k];
                    channel.times.push_back(static_cast<float>(key.time));
                    channel.values.emplace_back(static_cast<float>(key.value.x), static_cast<float>(key.value.y),
                                                static_cast<float>(key.value.z), static_cast<float>(key.value.w));
                }
                animation.duration = std::max(animation.duration, channel.times.back());
                animation.channels.push_back(std::move(channel));
            }
            if (!animation.channels.empty()) model_.animations.push_back(std::move(animation));
        }
    }

    static void addChannel(ImportedAnimation& animation, int node, AnimationChannel::Path path, const ufbx_baked_vec3_list& keys,
                           bool constant, glm::vec4 rest) {
        if (keys.count == 0) return;
        if (constant && glm::all(glm::lessThan(glm::abs(glm::vec4(toVec3(keys.data[0].value), 0.0f) - rest), glm::vec4(1e-5f)))) return;
        AnimationChannel channel;
        channel.node = node;
        channel.path = path;
        for (std::size_t k = 0; k < keys.count; ++k) {
            channel.times.push_back(static_cast<float>(keys.data[k].time));
            channel.values.emplace_back(toVec3(keys.data[k].value), 0.0f);
        }
        animation.duration = std::max(animation.duration, channel.times.back());
        animation.channels.push_back(std::move(channel));
    }

    const ufbx_scene& scene_;
    std::filesystem::path dir_;
    ImportedModel model_;
    std::vector<int> meshGroup_;  // per ufbx mesh: index into meshParts, -1 = no triangles
    std::unordered_map<const ufbx_node*, int> nodeIndex_;
    std::unordered_map<const ufbx_texture*, int> byTexture_;
    std::unordered_map<std::string, int> byFile_;
};

} // namespace

Result<ImportedModel> importFbx(const std::filesystem::path& file) {
    auto scene = load(file, false);
    if (!scene) return std::unexpected(scene.error());
    return Importer(**scene, file.parent_path()).run();
}

namespace {

std::vector<std::string> mtlLibraries(const std::filesystem::path& objFile) {
    std::vector<std::string> names;
    std::ifstream in(objFile);
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with("mtllib")) continue;
        std::string name = line.substr(6);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        if (!name.empty()) names.push_back(std::move(name));
    }
    return names;
}

} // namespace

std::vector<std::filesystem::path> fbxReferences(const std::filesystem::path& file) {
    std::vector<std::filesystem::path> out;
    std::unordered_set<std::string> seen;
    auto add = [&](std::filesystem::path relative) {
        relative = relative.lexically_normal();
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") relative = relative.filename();
        if (relative.empty() || relative == "..") return;
        if (seen.insert(utf8(relative)).second) out.push_back(std::move(relative));
    };
    if (lower(file.extension().string()) == ".obj") {
        for (const std::string& name : mtlLibraries(file)) add(utf8Path(name));
        add(utf8Path(utf8(file.stem()) + ".mtl"));
    }
    auto scene = load(file, true);
    if (!scene) return out;
    for (std::size_t t = 0; t < (*scene)->textures.count; ++t) {
        const ufbx_texture* texture = (*scene)->textures.data[t];
        if (texture->type != UFBX_TEXTURE_FILE || texture->content.size > 0) continue;
        const ufbx_string name = texture->relative_filename.length > 0 ? texture->relative_filename : texture->filename;
        if (name.length > 0) add(utf8Path(text(name)));
    }
    return out;
}

Result<std::vector<std::filesystem::path>> fbxDependencies(const std::filesystem::path& file) {
    auto scene = load(file, true);
    if (!scene) return std::unexpected(scene.error());
    const std::filesystem::path dir = file.parent_path();
    std::vector<std::filesystem::path> out;
    std::unordered_set<std::string> seen;
    auto add = [&](const std::filesystem::path& absolute) {
        std::filesystem::path relative = absolute.lexically_relative(dir).lexically_normal();
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return;  // outside the folder
        if (seen.insert(utf8(relative)).second) out.push_back(std::move(relative));
    };
    if (lower(file.extension().string()) == ".obj") {
        // Material libraries ("mtllib a.mtl"), and <name>.mtl, which ufbx also looks for.
        for (const std::string& name : mtlLibraries(file)) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(dir / utf8Path(name), ec)) add(dir / utf8Path(name));
        }
        std::filesystem::path sibling = file;
        sibling.replace_extension(".mtl");
        std::error_code ec;
        if (std::filesystem::is_regular_file(sibling, ec)) add(sibling);
    }
    for (std::size_t t = 0; t < (*scene)->textures.count; ++t) {
        const ufbx_texture* texture = (*scene)->textures.data[t];
        if (texture->type != UFBX_TEXTURE_FILE || texture->content.size > 0) continue;
        if (const auto found = findTextureFile(dir, *texture)) add(*found);
    }
    return out;
}

} // namespace lumora
