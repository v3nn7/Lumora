#include "assets/GltfImporter.hpp"

#include <cgltf.h>
#include <glm/geometric.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>
#include <format>
#include <memory>

namespace lumora {

namespace {

struct DataDeleter {
    void operator()(cgltf_data* d) const { cgltf_free(d); }
};
using DataPtr = std::unique_ptr<cgltf_data, DataDeleter>;

const char* resultName(cgltf_result r) {
    switch (r) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data too short";
        case cgltf_result_unknown_format: return "unknown format";
        case cgltf_result_invalid_json: return "invalid JSON";
        case cgltf_result_invalid_gltf: return "invalid glTF";
        case cgltf_result_invalid_options: return "invalid options";
        case cgltf_result_file_not_found: return "file not found";
        case cgltf_result_io_error: return "I/O error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "legacy glTF 1.0 is not supported";
        default: return "unknown error";
    }
}

Result<DataPtr> parse(const std::filesystem::path& file, bool loadBuffers) {
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    const std::string path = file.string();
    cgltf_result r = cgltf_parse_file(&options, path.c_str(), &raw);
    if (r != cgltf_result_success) {
        return makeError("cannot read glTF '{}': {}", path, resultName(r));
    }
    DataPtr data(raw);
    if (loadBuffers) {
        r = cgltf_load_buffers(&options, data.get(), path.c_str());
        if (r != cgltf_result_success) {
            return makeError("cannot load buffers of '{}': {}", path, resultName(r));
        }
        r = cgltf_validate(data.get());
        if (r != cgltf_result_success) {
            return makeError("'{}' failed glTF validation: {}", path, resultName(r));
        }
    }
    return data;
}

std::string nameOr(const char* name, const std::string& fallback) {
    return name != nullptr && *name != '\0' ? std::string(name) : fallback;
}

/// Appends one triangle primitive to `out`. Returns false (with a warning) if unsupported.
bool appendPrimitive(const cgltf_primitive& prim, MeshData& out, std::vector<std::string>& warnings,
                     const std::string& meshName) {
    if (prim.type != cgltf_primitive_type_triangles) {
        warnings.push_back(std::format("mesh '{}': skipped non-triangle primitive", meshName));
        return false;
    }
    const cgltf_accessor* positions = nullptr;
    const cgltf_accessor* normals = nullptr;
    const cgltf_accessor* uvs = nullptr;
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const cgltf_attribute& a = prim.attributes[i];
        if (a.type == cgltf_attribute_type_position) positions = a.data;
        if (a.type == cgltf_attribute_type_normal) normals = a.data;
        if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uvs = a.data;
    }
    if (positions == nullptr || positions->count == 0) {
        warnings.push_back(std::format("mesh '{}': primitive without positions skipped", meshName));
        return false;
    }
    if (prim.targets_count > 0) {
        warnings.push_back(std::format("mesh '{}': morph targets are ignored", meshName));
    }

    const auto base = static_cast<std::uint32_t>(out.vertices.size());
    const cgltf_size count = positions->count;
    out.vertices.resize(base + count);
    for (cgltf_size v = 0; v < count; ++v) {
        Vertex& vert = out.vertices[base + v];
        cgltf_accessor_read_float(positions, v, &vert.position.x, 3);
        if (normals != nullptr && v < normals->count) {
            cgltf_accessor_read_float(normals, v, &vert.normal.x, 3);
        } else {
            vert.normal = glm::vec3(0.0f);
        }
        if (uvs != nullptr && v < uvs->count) cgltf_accessor_read_float(uvs, v, &vert.uv.x, 2);
    }

    const std::size_t firstIndex = out.indices.size();
    if (prim.indices != nullptr) {
        out.indices.reserve(firstIndex + prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i) {
            const auto index = static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i));
            if (index >= count) {
                warnings.push_back(std::format("mesh '{}': out-of-range index dropped primitive", meshName));
                out.vertices.resize(base);
                out.indices.resize(firstIndex);
                return false;
            }
            out.indices.push_back(base + index);
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) out.indices.push_back(base + i);
    }
    // Trim a trailing partial triangle.
    out.indices.resize(firstIndex + (out.indices.size() - firstIndex) / 3 * 3);

    if (normals == nullptr) {
        // Area-weighted smooth normals from the triangles of this primitive.
        for (std::size_t t = firstIndex; t + 2 < out.indices.size(); t += 3) {
            Vertex& a = out.vertices[out.indices[t]];
            Vertex& b = out.vertices[out.indices[t + 1]];
            Vertex& c = out.vertices[out.indices[t + 2]];
            const glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
            a.normal += n;
            b.normal += n;
            c.normal += n;
        }
    }
    for (cgltf_size v = 0; v < count; ++v) {
        glm::vec3& n = out.vertices[base + v].normal;
        const float len = glm::length(n);
        n = len > 1e-12f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return true;
}

} // namespace

Result<ImportedModel> importGltf(const std::filesystem::path& file) {
    auto parsed = parse(file, true);
    if (!parsed) return std::unexpected(parsed.error());
    const cgltf_data& data = **parsed;
    ImportedModel model;

    for (cgltf_size m = 0; m < data.materials_count; ++m) {
        const cgltf_material& src = data.materials[m];
        ImportedMaterial mat;
        mat.name = nameOr(src.name, std::format("Material {}", m));
        if (src.has_pbr_metallic_roughness) {
            const auto& pbr = src.pbr_metallic_roughness;
            mat.baseColor = glm::vec4(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2],
                                      pbr.base_color_factor[3]);
            mat.metallic = pbr.metallic_factor;
            mat.roughness = pbr.roughness_factor;
            if (pbr.base_color_texture.texture != nullptr) {
                model.warnings.push_back(std::format("material '{}': textures are not imported yet", mat.name));
            }
        }
        mat.emissive = glm::vec3(src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]);
        model.materials.push_back(std::move(mat));
    }

    for (cgltf_size m = 0; m < data.meshes_count; ++m) {
        const cgltf_mesh& src = data.meshes[m];
        ImportedMesh mesh;
        mesh.name = nameOr(src.name, std::format("Mesh {}", m));
        for (cgltf_size p = 0; p < src.primitives_count; ++p) {
            const cgltf_primitive& prim = src.primitives[p];
            if (appendPrimitive(prim, mesh.data, model.warnings, mesh.name) && mesh.material < 0 &&
                prim.material != nullptr) {
                mesh.material = static_cast<int>(cgltf_material_index(&data, prim.material));
            }
        }
        if (src.primitives_count > 1) {
            model.warnings.push_back(std::format("mesh '{}': {} primitives merged (first material used)", mesh.name,
                                                 src.primitives_count));
        }
        mesh.data.bounds = computeBounds(mesh.data.vertices);
        model.meshes.push_back(std::move(mesh));
    }

    // Nodes of the default scene (or all root nodes), depth first so parents precede children.
    std::vector<const cgltf_node*> roots;
    if (const cgltf_scene* scene = data.scene != nullptr ? data.scene : (data.scenes_count > 0 ? data.scenes : nullptr)) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) roots.push_back(scene->nodes[i]);
    } else {
        for (cgltf_size i = 0; i < data.nodes_count; ++i) {
            if (data.nodes[i].parent == nullptr) roots.push_back(&data.nodes[i]);
        }
    }
    auto visit = [&](auto& self, const cgltf_node* node, int parent) -> void {
        ImportedNode out;
        out.name = nameOr(node->name, node->mesh != nullptr ? nameOr(node->mesh->name, "Node") : "Node");
        out.parent = parent;
        out.mesh = node->mesh != nullptr ? static_cast<int>(cgltf_mesh_index(&data, node->mesh)) : -1;
        if (node->has_matrix) {
            glm::mat4 m;
            std::memcpy(&m[0][0], node->matrix, sizeof(float) * 16);
            glm::vec3 skew;
            glm::vec4 perspective;
            glm::decompose(m, out.scale, out.rotation, out.translation, skew, perspective);
        } else {
            if (node->has_translation) out.translation = {node->translation[0], node->translation[1], node->translation[2]};
            if (node->has_rotation) {
                out.rotation = glm::quat(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]);
            }
            if (node->has_scale) out.scale = {node->scale[0], node->scale[1], node->scale[2]};
        }
        if (node->skin != nullptr) model.warnings.push_back(std::format("node '{}': skinning is ignored", out.name));
        const int index = static_cast<int>(model.nodes.size());
        model.nodes.push_back(std::move(out));
        for (cgltf_size c = 0; c < node->children_count; ++c) self(self, node->children[c], index);
    };
    for (const cgltf_node* root : roots) visit(visit, root, -1);
    return model;
}

Result<std::vector<std::filesystem::path>> gltfDependencies(const std::filesystem::path& file) {
    auto parsed = parse(file, false);
    if (!parsed) return std::unexpected(parsed.error());
    const cgltf_data& data = **parsed;
    std::vector<std::filesystem::path> deps;
    auto add = [&](const char* uri) {
        if (uri == nullptr || std::strncmp(uri, "data:", 5) == 0 || std::strstr(uri, "://") != nullptr) return;
        std::string decoded(uri);
        cgltf_decode_uri(decoded.data());
        decoded.resize(std::strlen(decoded.c_str()));
        deps.emplace_back(decoded);
    };
    for (cgltf_size i = 0; i < data.buffers_count; ++i) add(data.buffers[i].uri);
    for (cgltf_size i = 0; i < data.images_count; ++i) add(data.images[i].uri);
    return deps;
}

} // namespace lumora
