#include "assets/GltfImporter.hpp"

#include "assets/Image.hpp"
#include "assets/Primitives.hpp"
#include "core/FileSystem.hpp"

#include <cgltf.h>
#include <glm/geometric.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <unordered_map>
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
                     const std::string& meshName, bool& hasTangents) {
    if (prim.type != cgltf_primitive_type_triangles) {
        warnings.push_back(std::format("mesh '{}': skipped non-triangle primitive", meshName));
        return false;
    }
    const cgltf_accessor* positions = nullptr;
    const cgltf_accessor* normals = nullptr;
    const cgltf_accessor* uvs = nullptr;
    const cgltf_accessor* tangents = nullptr;
    const cgltf_accessor* joints = nullptr;
    const cgltf_accessor* weights = nullptr;
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const cgltf_attribute& a = prim.attributes[i];
        if (a.type == cgltf_attribute_type_joints && a.index == 0) joints = a.data;
        if (a.type == cgltf_attribute_type_weights && a.index == 0) weights = a.data;
        if (a.type == cgltf_attribute_type_position) positions = a.data;
        if (a.type == cgltf_attribute_type_normal) normals = a.data;
        if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uvs = a.data;
        if (a.type == cgltf_attribute_type_tangent) tangents = a.data;
    }
    if (positions == nullptr || positions->count == 0) {
        warnings.push_back(std::format("mesh '{}': primitive without positions skipped", meshName));
        return false;
    }
    hasTangents = hasTangents && tangents != nullptr;
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
        if (tangents != nullptr && v < tangents->count) cgltf_accessor_read_float(tangents, v, &vert.tangent.x, 4);
    }

    // Skin influences: kept for every vertex once any primitive of the mesh has them.
    const bool skinned = joints != nullptr && weights != nullptr;
    if (skinned || !out.skin.empty()) {
        out.skin.resize(base);  // earlier unskinned primitives follow joint 0 rigidly
        out.skin.resize(base + count);
        if (skinned) {
            for (cgltf_size v = 0; v < count && v < joints->count && v < weights->count; ++v) {
                SkinVertex& sv = out.skin[base + v];
                cgltf_uint j[4] = {0, 0, 0, 0};
                cgltf_accessor_read_uint(joints, v, j, 4);
                float w[4] = {0, 0, 0, 0};
                cgltf_accessor_read_float(weights, v, w, 4);
                const float sum = w[0] + w[1] + w[2] + w[3];
                for (int k = 0; k < 4; ++k) {
                    sv.joints[k] = static_cast<std::uint16_t>(j[k]);
                    sv.weights[k] = sum > 0.0f ? w[k] / sum : (k == 0 ? 1.0f : 0.0f);
                }
            }
        }
    }

    const std::size_t firstIndex = out.indices.size();
    if (prim.indices != nullptr) {
        out.indices.reserve(firstIndex + prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i) {
            const auto index = static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i));
            if (index >= count) {
                warnings.push_back(std::format("mesh '{}': out-of-range index dropped primitive", meshName));
                out.vertices.resize(base);
                if (!out.skin.empty()) out.skin.resize(base);
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

/// Encoded bytes of an image: buffer view (.glb), base64 data URI, or external file.
Result<std::vector<std::byte>> imageBytes(const cgltf_image& image, const std::filesystem::path& modelDir) {
    if (image.buffer_view != nullptr) {
        const cgltf_buffer_view& view = *image.buffer_view;
        if (view.buffer == nullptr || view.buffer->data == nullptr || view.offset + view.size > view.buffer->size) {
            return makeError("image buffer view is out of range");
        }
        const auto* begin = static_cast<const std::byte*>(view.buffer->data) + view.offset;
        return std::vector<std::byte>(begin, begin + view.size);
    }
    if (image.uri == nullptr) return makeError("image has neither a URI nor a buffer view");
    if (std::strncmp(image.uri, "data:", 5) == 0) {
        const char* comma = std::strchr(image.uri, ',');
        if (comma == nullptr || std::strstr(image.uri, ";base64,") == nullptr) return makeError("unsupported data URI");
        const char* b64 = comma + 1;
        const std::size_t length = std::strlen(b64);
        std::size_t padding = 0;
        if (length >= 1 && b64[length - 1] == '=') ++padding;
        if (length >= 2 && b64[length - 2] == '=') ++padding;
        const std::size_t size = length / 4 * 3 - padding;
        void* decoded = nullptr;
        cgltf_options options{};
        if (cgltf_load_buffer_base64(&options, size, b64, &decoded) != cgltf_result_success) {
            return makeError("invalid base64 image data");
        }
        std::vector<std::byte> bytes(static_cast<const std::byte*>(decoded), static_cast<const std::byte*>(decoded) + size);
        std::free(decoded);
        return bytes;
    }
    std::string relative(image.uri);
    cgltf_decode_uri(relative.data());
    relative.resize(std::strlen(relative.c_str()));
    const std::filesystem::path path = (modelDir / relative).lexically_normal();
    return fs::readBinaryFile(path);
}

int imageIndexOf(const cgltf_data& data, const cgltf_texture_view& view, std::vector<std::string>& warnings,
                 const std::string& material) {
    if (view.texture == nullptr) return -1;
    if (view.texcoord != 0) {
        warnings.push_back(std::format("material '{}': texture uses UV set {} (only set 0 is supported)", material,
                                       view.texcoord));
    }
    if (view.texture->image == nullptr) {
        warnings.push_back(std::format("material '{}': texture without a PNG/JPEG image (e.g. KTX2/WebP) skipped",
                                       material));
        return -1;
    }
    return static_cast<int>(cgltf_image_index(&data, view.texture->image));
}

} // namespace

float findPaddedBackground(const ImageData& image, std::vector<std::uint8_t>* outMask) {
    const std::uint32_t w = image.width, h = image.height;
    if (w < 8 || h < 8) return 0.0f;
    constexpr int kTolerance = 2;      // per channel: "identical" after compression noise
    constexpr std::uint32_t kRun = 8;  // padding streaks are at least this long
    const auto at = [&](std::uint32_t x, std::uint32_t y) { return &image.rgba[(static_cast<std::size_t>(y) * w + x) * 4]; };
    const auto same = [&](const std::uint8_t* a, const std::uint8_t* b) {
        return std::abs(a[0] - b[0]) <= kTolerance && std::abs(a[1] - b[1]) <= kTolerance && std::abs(a[2] - b[2]) <= kTolerance;
    };
    std::vector<std::uint8_t> candidate(static_cast<std::size_t>(w) * h, 0);
    for (std::uint32_t y = 0; y < h; ++y) {  // white
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t* p = at(x, y);
            if (std::min({p[0], p[1], p[2]}) > 225) candidate[static_cast<std::size_t>(y) * w + x] = 1;
        }
    }
    for (std::uint32_t y = 0; y < h; ++y) {  // horizontal streaks
        for (std::uint32_t x = 0; x < w;) {
            std::uint32_t end = x + 1;
            while (end < w && same(at(end - 1, y), at(end, y))) ++end;
            if (end - x >= kRun) std::fill_n(candidate.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * w + x), end - x, 1);
            x = end;
        }
    }
    for (std::uint32_t x = 0; x < w; ++x) {  // vertical streaks
        for (std::uint32_t y = 0; y < h;) {
            std::uint32_t end = y + 1;
            while (end < h && same(at(x, end - 1), at(x, end))) ++end;
            if (end - y >= kRun) {
                for (std::uint32_t k = y; k < end; ++k) candidate[static_cast<std::size_t>(k) * w + x] = 1;
            }
            y = end;
        }
    }
    // Background = candidates connected to the border; flat patches inside a leaf stay leaf.
    std::vector<std::uint8_t> background(candidate.size(), 0);
    std::vector<std::uint32_t> queue;
    auto seed = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (candidate[i] && !background[i]) {
            background[i] = 1;
            queue.push_back(static_cast<std::uint32_t>(i));
        }
    };
    std::size_t borderCandidates = 0;
    for (std::uint32_t x = 0; x < w; ++x) {
        borderCandidates += candidate[x] + candidate[static_cast<std::size_t>(h - 1) * w + x];
        seed(x, 0);
        seed(x, h - 1);
    }
    for (std::uint32_t y = 0; y < h; ++y) {
        borderCandidates += candidate[static_cast<std::size_t>(y) * w] + candidate[static_cast<std::size_t>(y) * w + w - 1];
        seed(0, y);
        seed(w - 1, y);
    }
    for (std::size_t q = 0; q < queue.size(); ++q) {
        const std::uint32_t x = queue[q] % w, y = queue[q] / w;
        if (x > 0) seed(x - 1, y);
        if (x + 1 < w) seed(x + 1, y);
        if (y > 0) seed(x, y - 1);
        if (y + 1 < h) seed(x, y + 1);
    }
    // Only a map whose border is mostly background has one.
    if (borderCandidates * 4 < (2 * static_cast<std::size_t>(w) + 2 * h) * 3) return 0.0f;
    const float share = static_cast<float>(queue.size()) / static_cast<float>(background.size());
    if (outMask != nullptr) *outMask = std::move(background);
    return share;
}

void cutOutPaddedBackground(ImageData& image) {
    std::vector<std::uint8_t> background;
    if (findPaddedBackground(image, &background) <= 0.0f) return;
    for (std::size_t i = 0; i < background.size(); ++i) {
        if (background[i]) image.rgba[i * 4 + 3] = 0;
    }
}

float importedAlphaCutoff(const ImportedMaterial& m) {
    if (m.baseColorTexture < 0) return 0.0f;
    float cutoff = 0.0f;
    if (m.alphaMode == ImportedMaterial::AlphaMode::Mask) cutoff = std::clamp(m.alphaCutoff, 0.01f, 1.0f);
    else if (m.alphaMode == ImportedMaterial::AlphaMode::Blend || m.baseColorHasAlpha) cutoff = 0.5f;
    // The factor's alpha scales the map's: map alpha x factor < cutoff, i.e. map alpha < cutoff / factor.
    if (cutoff > 0.0f && m.baseColor.a > 0.0f && m.baseColor.a < 1.0f) cutoff = std::min(cutoff / m.baseColor.a, 1.0f);
    return cutoff;
}

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
            mat.baseColorTexture = imageIndexOf(data, pbr.base_color_texture, model.warnings, mat.name);
            mat.metallicRoughnessTexture = imageIndexOf(data, pbr.metallic_roughness_texture, model.warnings, mat.name);
        }
        mat.emissive = glm::vec3(src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]);
        if (src.has_emissive_strength) mat.emissive *= src.emissive_strength.emissive_strength;
        mat.emissiveTexture = imageIndexOf(data, src.emissive_texture, model.warnings, mat.name);
        mat.normalTexture = imageIndexOf(data, src.normal_texture, model.warnings, mat.name);
        mat.normalScale = src.normal_texture.texture != nullptr ? src.normal_texture.scale : 1.0f;
        mat.occlusionTexture = imageIndexOf(data, src.occlusion_texture, model.warnings, mat.name);
        mat.alphaMode = src.alpha_mode == cgltf_alpha_mode_mask    ? ImportedMaterial::AlphaMode::Mask
                        : src.alpha_mode == cgltf_alpha_mode_blend ? ImportedMaterial::AlphaMode::Blend
                                                                   : ImportedMaterial::AlphaMode::Opaque;
        mat.alphaCutoff = src.alpha_cutoff;
        mat.doubleSided = src.double_sided != 0;
        model.materials.push_back(std::move(mat));
    }

    for (cgltf_size i = 0; i < data.images_count; ++i) {
        const cgltf_image& src = data.images[i];
        ImportedImage image;
        image.name = nameOr(src.name, std::format("Image {}", i));
        image.mimeType = src.mime_type != nullptr ? src.mime_type : "";
        if (auto bytes = imageBytes(src, file.parent_path())) {
            image.encoded = std::move(*bytes);
        } else {
            model.warnings.push_back(std::format("image '{}': {}", image.name, bytes.error().message));
        }
        model.images.push_back(std::move(image));
    }

    // One ImportedMesh per material: glTF mesh m keeps index m for its first material's
    // primitives (so "file#m" keys stay valid); further materials become extra meshes appended
    // after all glTF meshes and listed in meshParts[m].
    std::vector<ImportedMesh> extraParts;
    model.meshParts.resize(data.meshes_count);
    for (cgltf_size m = 0; m < data.meshes_count; ++m) {
        const cgltf_mesh& src = data.meshes[m];
        const std::string baseName = nameOr(src.name, std::format("Mesh {}", m));
        std::vector<int> materials;  // in order of first appearance; -1 = no material
        for (cgltf_size p = 0; p < src.primitives_count; ++p) {
            const int material = src.primitives[p].material != nullptr
                                     ? static_cast<int>(cgltf_material_index(&data, src.primitives[p].material))
                                     : -1;
            if (std::ranges::find(materials, material) == materials.end()) materials.push_back(material);
        }
        if (materials.empty()) materials.push_back(-1);
        for (std::size_t g = 0; g < materials.size(); ++g) {
            ImportedMesh mesh;
            mesh.name = g == 0 ? baseName : std::format("{} ({})", baseName, g);
            mesh.material = materials[g];
            bool hasTangents = true;
            for (cgltf_size p = 0; p < src.primitives_count; ++p) {
                const cgltf_primitive& prim = src.primitives[p];
                const int material = prim.material != nullptr ? static_cast<int>(cgltf_material_index(&data, prim.material)) : -1;
                if (material == materials[g]) appendPrimitive(prim, mesh.data, model.warnings, mesh.name, hasTangents);
            }
            if (!hasTangents) generateTangents(mesh.data);
            mesh.data.bounds = computeBounds(mesh.data.vertices);
            if (g == 0) {
                model.meshParts[m].push_back(static_cast<int>(model.meshes.size()));
                model.meshes.push_back(std::move(mesh));
            } else {
                model.meshParts[m].push_back(-1 - static_cast<int>(extraParts.size()));  // resolved below
                extraParts.push_back(std::move(mesh));
            }
        }
    }
    const int glTfMeshes = static_cast<int>(model.meshes.size());
    for (auto& parts : model.meshParts) {
        for (int& part : parts) {
            if (part < 0) part = glTfMeshes + (-1 - part);
        }
    }
    for (ImportedMesh& mesh : extraParts) model.meshes.push_back(std::move(mesh));

    // Nodes of the default scene (or all root nodes), depth first so parents precede children.
    std::vector<const cgltf_node*> roots;
    if (const cgltf_scene* scene = data.scene != nullptr ? data.scene : (data.scenes_count > 0 ? data.scenes : nullptr)) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) roots.push_back(scene->nodes[i]);
    } else {
        for (cgltf_size i = 0; i < data.nodes_count; ++i) {
            if (data.nodes[i].parent == nullptr) roots.push_back(&data.nodes[i]);
        }
    }
    std::unordered_map<const cgltf_node*, int> nodeIndex;
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
        if (node->skin != nullptr) out.skin = static_cast<int>(cgltf_skin_index(&data, node->skin));
        const int index = static_cast<int>(model.nodes.size());
        nodeIndex[node] = index;
        model.nodes.push_back(std::move(out));
        for (cgltf_size c = 0; c < node->children_count; ++c) self(self, node->children[c], index);
    };
    for (const cgltf_node* root : roots) visit(visit, root, -1);

    // Base colour maps with transparency (PNG colour type 4/6, or a tRNS chunk), without decoding.
    auto pngHasAlpha = [](const std::vector<std::byte>& png) {
        static constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (png.size() < 33 || std::memcmp(png.data(), kSignature, 8) != 0) return false;
        const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(png[i]); };
        const unsigned colorType = byte(25);
        if (colorType == 4 || colorType == 6) return true;
        for (std::size_t at = 8; at + 8 <= png.size();) {  // chunks up to the image data
            const std::size_t length = (std::size_t{byte(at)} << 24) | (std::size_t{byte(at + 1)} << 16) |
                                       (std::size_t{byte(at + 2)} << 8) | byte(at + 3);
            const std::string type(reinterpret_cast<const char*>(png.data()) + at + 4, 4);
            if (type == "tRNS") return true;
            if (type == "IDAT" || type == "IEND") return false;
            at += 12 + length;
        }
        return false;
    };
    // Foliage maps that lost their alpha (common in SketchUp models): cut out the background
    // (white, or the flat streaks exporters pad with). Only for leaf-like names and only when
    // the map's border is background, so walls, snow or paper keep their colour.
    auto foliageName = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* word : {"leaf", "leaves", "foliage", "grass", "plant", "tree", "branch", "fern", "bush", "ivy",
                                 "flower", "hedge", "palm", "needle", "shrub", "twig", "vine"}) {
            if (text.find(word) != std::string::npos) return true;
        }
        return false;
    };
    auto paddedBackground = [](const std::vector<std::byte>& encoded) {
        auto decoded = decodeImage(encoded);
        if (!decoded) return false;
        const float share = findPaddedBackground(*decoded);
        return share > 0.05f && share < 0.95f;  // some background, and something left
    };
    for (ImportedMaterial& mat : model.materials) {
        if (mat.baseColorTexture < 0 || static_cast<std::size_t>(mat.baseColorTexture) >= model.images.size()) continue;
        ImportedImage& image = model.images[static_cast<std::size_t>(mat.baseColorTexture)];
        mat.baseColorHasAlpha = pngHasAlpha(image.encoded);
        if (!mat.baseColorHasAlpha && mat.alphaMode == ImportedMaterial::AlphaMode::Opaque &&
            (foliageName(mat.name) || foliageName(image.name)) && paddedBackground(image.encoded)) {
            image.cutOutBackground = true;
            mat.baseColorHasAlpha = true;  // cut out like a map with alpha
            model.warnings.push_back(std::format("material '{}': leaf map without alpha, background cut out", mat.name));
        }
    }

    // Extent of the whole model, and the units it really uses.
    {
        std::vector<glm::mat4> world(model.nodes.size());
        bool any = false;
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const ImportedNode& n = model.nodes[i];
            const glm::mat4 local = glm::translate(glm::mat4(1.0f), n.translation) * glm::mat4_cast(n.rotation) *
                                    glm::scale(glm::mat4(1.0f), n.scale);
            world[i] = n.parent >= 0 ? world[static_cast<std::size_t>(n.parent)] * local : local;
            if (n.mesh < 0 || static_cast<std::size_t>(n.mesh) >= model.meshParts.size()) continue;
            for (int part : model.meshParts[static_cast<std::size_t>(n.mesh)]) {
                if (part < 0 || static_cast<std::size_t>(part) >= model.meshes.size()) continue;
                const Aabb& b = model.meshes[static_cast<std::size_t>(part)].data.bounds;
                for (int c = 0; c < 8; ++c) {
                    const glm::vec3 corner((c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z);
                    const glm::vec3 p(world[i] * glm::vec4(corner, 1.0f));
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                    any = true;
                }
            }
        }
        if (any) {
            model.boundsMin = lo;
            model.boundsMax = hi;
        }
        // SketchUp works in inches; its Collada (and models converted from it, e.g. on Sketchfab)
        // keep those numbers as metres.
        const std::string generator = data.asset.generator != nullptr ? data.asset.generator : "";
        const bool sketchUp = generator.find("SketchUp") != std::string::npos ||
                              std::ranges::any_of(model.nodes, [](const ImportedNode& n) { return n.name == "SketchUp"; });
        if (sketchUp) {
            model.unitScale = 0.0254f;
            model.warnings.push_back("made with SketchUp: inches, scaled to metres (x 0.0254)");
            // SketchUp shows both sides of every face; its models rely on that.
            for (ImportedMaterial& mat : model.materials) mat.doubleSided = true;
        }
    }

    // Skins: joints as indices of the imported nodes.
    for (cgltf_size k = 0; k < data.skins_count; ++k) {
        const cgltf_skin& source = data.skins[k];
        ImportedSkin skin;
        skin.name = nameOr(source.name, std::format("Skin {}", k));
        bool complete = true;
        for (cgltf_size j = 0; j < source.joints_count; ++j) {
            const auto found = nodeIndex.find(source.joints[j]);
            if (found == nodeIndex.end()) complete = false;
            skin.joints.push_back(found != nodeIndex.end() ? found->second : -1);
            glm::mat4 inverseBind(1.0f);
            if (source.inverse_bind_matrices != nullptr && j < source.inverse_bind_matrices->count) {
                cgltf_accessor_read_float(source.inverse_bind_matrices, j, &inverseBind[0][0], 16);
            }
            skin.inverseBind.push_back(inverseBind);
        }
        if (!complete) model.warnings.push_back(std::format("skin '{}': joints outside the scene", skin.name));
        model.skins.push_back(std::move(skin));
    }

    // Animations: channels on nodes of the imported scene. Weights (morph targets) are not supported.
    for (cgltf_size a = 0; a < data.animations_count; ++a) {
        const cgltf_animation& source = data.animations[a];
        ImportedAnimation animation;
        animation.name = nameOr(source.name, std::format("Animation {}", a));
        for (cgltf_size c = 0; c < source.channels_count; ++c) {
            const cgltf_animation_channel& ch = source.channels[c];
            const auto found = nodeIndex.find(ch.target_node);
            if (found == nodeIndex.end() || ch.sampler == nullptr || ch.sampler->input == nullptr ||
                ch.sampler->output == nullptr) {
                continue;
            }
            AnimationChannel channel;
            channel.node = found->second;
            switch (ch.target_path) {
                case cgltf_animation_path_type_translation: channel.path = AnimationChannel::Path::Translation; break;
                case cgltf_animation_path_type_rotation: channel.path = AnimationChannel::Path::Rotation; break;
                case cgltf_animation_path_type_scale: channel.path = AnimationChannel::Path::Scale; break;
                default:
                    model.warnings.push_back(std::format("animation '{}': morph target weights are ignored", animation.name));
                    continue;
            }
            switch (ch.sampler->interpolation) {
                case cgltf_interpolation_type_step: channel.interpolation = AnimationChannel::Interpolation::Step; break;
                case cgltf_interpolation_type_cubic_spline:
                    channel.interpolation = AnimationChannel::Interpolation::CubicSpline;
                    break;
                default: channel.interpolation = AnimationChannel::Interpolation::Linear; break;
            }
            const cgltf_accessor* input = ch.sampler->input;
            const cgltf_accessor* output = ch.sampler->output;
            channel.times.resize(input->count);
            for (cgltf_size k = 0; k < input->count; ++k) cgltf_accessor_read_float(input, k, &channel.times[k], 1);
            const cgltf_size components = channel.path == AnimationChannel::Path::Rotation ? 4 : 3;
            channel.values.resize(output->count, glm::vec4(0.0f));
            for (cgltf_size k = 0; k < output->count; ++k) {
                cgltf_accessor_read_float(output, k, &channel.values[k].x, components);
            }
            const std::size_t perKey = channel.interpolation == AnimationChannel::Interpolation::CubicSpline ? 3 : 1;
            if (channel.times.empty() || channel.values.size() != channel.times.size() * perKey) {
                model.warnings.push_back(std::format("animation '{}': malformed channel skipped", animation.name));
                continue;
            }
            animation.duration = std::max(animation.duration, channel.times.back());
            animation.channels.push_back(std::move(channel));
        }
        if (!animation.channels.empty()) model.animations.push_back(std::move(animation));
    }
    return model;
}

void sampleAnimation(const ImportedAnimation& animation, float time, std::vector<NodePose>& poses) {
    for (const AnimationChannel& ch : animation.channels) {
        if (ch.node < 0 || static_cast<std::size_t>(ch.node) >= poses.size()) continue;
        const std::vector<float>& t = ch.times;
        // Key interval containing the time (clamped to the first and last key).
        std::size_t k1 = static_cast<std::size_t>(std::upper_bound(t.begin(), t.end(), time) - t.begin());
        std::size_t k0 = k1 == 0 ? 0 : k1 - 1;
        k1 = std::min(k1, t.size() - 1);
        const float span = t[k1] - t[k0];
        const float u = span > 0.0f ? std::clamp((time - t[k0]) / span, 0.0f, 1.0f) : 0.0f;
        glm::vec4 value;
        const bool rotation = ch.path == AnimationChannel::Path::Rotation;
        switch (ch.interpolation) {
            case AnimationChannel::Interpolation::Step:
                value = ch.values[k0];
                break;
            case AnimationChannel::Interpolation::CubicSpline: {
                // Hermite spline: values are (in-tangent, value, out-tangent) per key.
                const glm::vec4 p0 = ch.values[k0 * 3 + 1];
                const glm::vec4 m0 = ch.values[k0 * 3 + 2] * span;
                const glm::vec4 p1 = ch.values[k1 * 3 + 1];
                const glm::vec4 m1 = ch.values[k1 * 3 + 0] * span;
                const float u2 = u * u;
                const float u3 = u2 * u;
                value = (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
                break;
            }
            default:
                if (rotation) {
                    const glm::quat a(ch.values[k0].w, ch.values[k0].x, ch.values[k0].y, ch.values[k0].z);
                    const glm::quat b(ch.values[k1].w, ch.values[k1].x, ch.values[k1].y, ch.values[k1].z);
                    const glm::quat q = glm::slerp(a, b, u);
                    value = glm::vec4(q.x, q.y, q.z, q.w);
                } else {
                    value = glm::mix(ch.values[k0], ch.values[k1], u);
                }
                break;
        }
        NodePose& pose = poses[static_cast<std::size_t>(ch.node)];
        switch (ch.path) {
            case AnimationChannel::Path::Translation: pose.translation = glm::vec3(value); break;
            case AnimationChannel::Path::Scale: pose.scale = glm::vec3(value); break;
            case AnimationChannel::Path::Rotation:
                pose.rotation = glm::normalize(glm::quat(value.w, value.x, value.y, value.z));
                break;
        }
    }
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
