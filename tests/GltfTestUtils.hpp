#pragma once

#include "assets/Primitives.hpp"

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace lumora::test {

inline std::string base64(const std::vector<unsigned char>& data) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const unsigned v = (data[i] << 16) | ((i + 1 < data.size() ? data[i + 1] : 0) << 8) |
                           (i + 2 < data.size() ? data[i + 2] : 0);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += i + 1 < data.size() ? kTable[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kTable[v & 63] : '=';
    }
    return out;
}

/// Writes a glTF 2.0 file containing one cube mesh (positions, normals, uint16 indices) under a
/// two-level node hierarchy, with a red material. `externalBuffer` stores the binary data in
/// "data/cube.bin" next to the file instead of a data URI.
inline void writeCubeGltf(const std::filesystem::path& file, bool externalBuffer) {
    const MeshData cube = makeCube(1.0f);
    std::vector<unsigned char> bin;
    auto append = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        bin.insert(bin.end(), b, b + n);
    };
    for (const Vertex& v : cube.vertices) append(&v.position, 12);
    const std::size_t normalOffset = bin.size();
    for (const Vertex& v : cube.vertices) append(&v.normal, 12);
    const std::size_t indexOffset = bin.size();
    for (std::uint32_t i : cube.indices) {
        const auto s = static_cast<std::uint16_t>(i);
        append(&s, 2);
    }
    const std::size_t total = bin.size();
    std::string uri = "data/cube.bin";
    if (externalBuffer) {
        std::filesystem::create_directories(file.parent_path() / "data");
        std::ofstream(file.parent_path() / uri, std::ios::binary).write(reinterpret_cast<const char*>(bin.data()),
                                                                        static_cast<std::streamsize>(bin.size()));
    } else {
        uri = "data:application/octet-stream;base64," + base64(bin);
    }
    const std::size_t n = cube.vertices.size();
    std::ofstream(file) << std::format(R"({{
  "asset": {{"version": "2.0"}},
  "scene": 0,
  "scenes": [{{"nodes": [0]}}],
  "nodes": [
    {{"name": "Root", "translation": [0, 1, 0], "children": [1]}},
    {{"name": "Box", "mesh": 0, "rotation": [0, 0.7071068, 0, 0.7071068], "scale": [2, 2, 2]}}
  ],
  "meshes": [{{"name": "CubeMesh", "primitives": [{{"attributes": {{"POSITION": 0, "NORMAL": 1}}, "indices": 2, "material": 0}}]}}],
  "materials": [{{"name": "Red", "pbrMetallicRoughness": {{"baseColorFactor": [0.9, 0.1, 0.05, 1], "metallicFactor": 0.0, "roughnessFactor": 0.5}}}}],
  "buffers": [{{"byteLength": {}, "uri": "{}"}}],
  "bufferViews": [
    {{"buffer": 0, "byteOffset": 0, "byteLength": {}}},
    {{"buffer": 0, "byteOffset": {}, "byteLength": {}}},
    {{"buffer": 0, "byteOffset": {}, "byteLength": {}}}
  ],
  "accessors": [
    {{"bufferView": 0, "componentType": 5126, "count": {}, "type": "VEC3", "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]}},
    {{"bufferView": 1, "componentType": 5126, "count": {}, "type": "VEC3"}},
    {{"bufferView": 2, "componentType": 5123, "count": {}, "type": "SCALAR"}}
  ]
}})",
                                       total, uri, n * 12, normalOffset, n * 12, indexOffset, cube.indices.size() * 2, n,
                                       n, cube.indices.size());
}

} // namespace lumora::test
