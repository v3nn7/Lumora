#pragma once

#include "assets/Primitives.hpp"

#include <stb_image_write.h>

#include <cmath>
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

/// Solid-colour PNG (RGBA8) encoded in memory.
inline std::vector<unsigned char> solidPng(int width, int height, unsigned char r, unsigned char g, unsigned char b) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    std::vector<unsigned char> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<unsigned char>*>(context);
            out->insert(out->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
        },
        &png, width, height, 4, pixels.data(), width * 4);
    return png;
}

/// Writes a glTF 2.0 file containing one cube mesh (positions, normals, uint16 indices) under a
/// two-level node hierarchy, with a red material. `externalBuffer` stores the binary data in
/// "data/cube.bin" next to the file instead of a data URI.
inline void writeCubeGltf(const std::filesystem::path& file, bool externalBuffer, bool greenTexture = false) {
    const MeshData cube = makeCube(1.0f);
    std::vector<unsigned char> bin;
    auto append = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        bin.insert(bin.end(), b, b + n);
    };
    for (const Vertex& v : cube.vertices) append(&v.position, 12);
    const std::size_t normalOffset = bin.size();
    for (const Vertex& v : cube.vertices) append(&v.normal, 12);
    const std::size_t uvOffset = bin.size();
    for (const Vertex& v : cube.vertices) append(&v.uv, 8);
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
    const std::string material = greenTexture
        ? R"("materials": [{"name": "Textured", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.6}}],
  "textures": [{"source": 0}],
  "images": [{"name": "Green", "uri": "data:image/png;base64,)" + base64(solidPng(8, 8, 20, 220, 40)) + R"("}],)"
        : R"("materials": [{"name": "Red", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.1, 0.05, 1], "metallicFactor": 0.0, "roughnessFactor": 0.5}}],)";
    std::ofstream(file) << std::format(R"({{
  "asset": {{"version": "2.0"}},
  "scene": 0,
  "scenes": [{{"nodes": [0]}}],
  "nodes": [
    {{"name": "Root", "translation": [0, 1, 0], "children": [1]}},
    {{"name": "Box", "mesh": 0, "rotation": [0, 0.7071068, 0, 0.7071068], "scale": [2, 2, 2]}}
  ],
  "meshes": [{{"name": "CubeMesh", "primitives": [{{"attributes": {{"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 3}}, "indices": 2, "material": 0}}]}}],
  {}
  "buffers": [{{"byteLength": {}, "uri": "{}"}}],
  "bufferViews": [
    {{"buffer": 0, "byteOffset": 0, "byteLength": {}}},
    {{"buffer": 0, "byteOffset": {}, "byteLength": {}}},
    {{"buffer": 0, "byteOffset": {}, "byteLength": {}}},
    {{"buffer": 0, "byteOffset": {}, "byteLength": {}}}
  ],
  "accessors": [
    {{"bufferView": 0, "componentType": 5126, "count": {}, "type": "VEC3", "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]}},
    {{"bufferView": 1, "componentType": 5126, "count": {}, "type": "VEC3"}},
    {{"bufferView": 2, "componentType": 5123, "count": {}, "type": "SCALAR"}},
    {{"bufferView": 3, "componentType": 5126, "count": {}, "type": "VEC2"}}
  ]
}})",
                                       material, total, uri, n * 12, normalOffset, n * 12, indexOffset,
                                       cube.indices.size() * 2, uvOffset, n * 8, n, n, cube.indices.size(), n);
}

} // namespace lumora::test

namespace lumora::test {

/// Writes a glTF with two nodes and no meshes: "Arm" (root) and its child "Hand", and two clips:
/// "Slide" moves Arm from x = 0 to x = 2 over 1 s (linear) and turns Hand 90 degrees about +Y at
/// 0.5 s (step); "Lift" raises Arm to y = 3 over 2 s.
inline void writeAnimatedGltf(const std::filesystem::path& file) {
    std::vector<unsigned char> bin;
    auto floats = [&](std::initializer_list<float> values) {
        const std::size_t offset = bin.size();
        for (float f : values) {
            const auto* b = reinterpret_cast<const unsigned char*>(&f);
            bin.insert(bin.end(), b, b + 4);
        }
        return offset;
    };
    const float s = std::sqrt(0.5f);
    const std::size_t times01 = floats({0.0f, 1.0f});
    const std::size_t slide = floats({0, 0, 0, 2, 0, 0});
    const std::size_t times0_05 = floats({0.0f, 0.5f});
    const std::size_t turn = floats({0, 0, 0, 1, 0, s, 0, s});
    const std::size_t times02 = floats({0.0f, 2.0f});
    const std::size_t lift = floats({0, 0, 0, 0, 3, 0});
    auto view = [&](std::size_t offset, std::size_t length) {
        return std::format(R"({{"buffer":0,"byteOffset":{},"byteLength":{}}})", offset, length);
    };
    const std::string json = std::format(R"({{
  "asset": {{"version": "2.0"}},
  "scene": 0,
  "scenes": [{{"nodes": [0]}}],
  "nodes": [{{"name": "Arm", "children": [1]}}, {{"name": "Hand", "translation": [0, 1, 0]}}],
  "buffers": [{{"byteLength": {}, "uri": "data:application/octet-stream;base64,{}"}}],
  "bufferViews": [{}, {}, {}, {}, {}, {}],
  "accessors": [
    {{"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]}},
    {{"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}},
    {{"bufferView": 2, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [0.5]}},
    {{"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC4"}},
    {{"bufferView": 4, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [2]}},
    {{"bufferView": 5, "componentType": 5126, "count": 2, "type": "VEC3"}}
  ],
  "animations": [
    {{"name": "Slide",
      "samplers": [{{"input": 0, "output": 1, "interpolation": "LINEAR"}}, {{"input": 2, "output": 3, "interpolation": "STEP"}}],
      "channels": [{{"sampler": 0, "target": {{"node": 0, "path": "translation"}}}},
                   {{"sampler": 1, "target": {{"node": 1, "path": "rotation"}}}}]}},
    {{"name": "Lift",
      "samplers": [{{"input": 4, "output": 5}}],
      "channels": [{{"sampler": 0, "target": {{"node": 0, "path": "translation"}}}}]}}
  ]
}})",
        bin.size(), base64(bin), view(times01, 8), view(slide, 24), view(times0_05, 8), view(turn, 32), view(times02, 8),
        view(lift, 24));
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << json;
}

} // namespace lumora::test
