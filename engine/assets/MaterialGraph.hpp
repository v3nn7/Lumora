#pragma once

#include "core/Error.hpp"

#include <glm/vec4.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lumora {

/// Material graphs (.lmgraph): nodes joined by links compute a surface per pixel (and an offset
/// per vertex, e.g. wind), saved in the same JSON layout as visual scripts. They compile to a
/// small program that one shader interprets on the GPU (shaders/material_graph.hlsli), so edits
/// show at once without a shader compiler.
namespace matgraph {

enum class PinKind : std::uint8_t { Float, Vector, Bool, Texture };

struct Pin {
    std::string_view name;
    PinKind kind = PinKind::Float;
    std::string_view label;  // empty = name
    float x = 0.0f, y = 0.0f, z = 0.0f;  // default (Float: x; Vector: x, y, z; Bool: x != 0)
    std::string_view defaultText;       // Texture: default asset key (usually empty)
};

struct NodeType {
    std::string_view type;  // "math.multiply"
    std::string_view title;
    std::string_view category;
    std::string_view description;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
};

/// Every node type, in menu order. "output.material" is the one the surface comes out of.
[[nodiscard]] const std::vector<NodeType>& nodeTypes();
[[nodiscard]] const NodeType* findNodeType(std::string_view type);

inline constexpr std::string_view kOutputNode = "output.material";
inline constexpr std::uint32_t kRegisters = 32;     // float4 registers of the interpreter
inline constexpr std::uint32_t kMaxInstructions = 256;
inline constexpr std::uint32_t kMaxTextures = 8;

/// Registers holding the results (the program writes every one of its stage).
enum Output : std::uint32_t {
    kBaseColor = 0,
    kOpacity = 1,
    kMetallic = 2,
    kRoughness = 3,
    kNormal = 4,  // tangent space
    kEmissive = 5,
    kOcclusion = 6,
    kWorldOffset = 7,  // the vertex program's
    kFirstTemporary = 8,
};

/// Interpreter operations (shaders/material_graph.hlsli has the same numbers). Each instruction
/// is two float4 texels: (op, dst, a, b), (c, p0, p1, p2); CONST keeps its value in the second.
enum class Op : std::uint32_t {
    Const = 1,
    Input = 2,  // p0: 0 uv0, 1 uv1, 2 world position, 3 world normal, 4 view direction, 5 time, 6 local position
    Texture = 3,  // a: uv; p0: slot; p1: 0 rgba, 1 normal map (b: strength)
    Add = 4,
    Subtract = 5,
    Multiply = 6,
    Divide = 7,
    Min = 8,
    Max = 9,
    Power = 10,
    Lerp = 11,
    Clamp = 12,
    OneMinus = 13,
    Abs = 14,
    Frac = 15,
    Sine = 16,
    Dot = 17,
    Splat = 18,  // p0: component
    Combine = 19,
    Saturate = 20,
    Fresnel = 21,
    Slope = 22,
    Noise = 23,
    Wind = 24,
    Tiling = 25,
    Panner = 26,
    Move = 27,
    Length = 28,
    Smoothstep = 29,
    Normalize = 30,
};

enum class TextureKind : std::uint8_t { Color, Data, Normal };

} // namespace matgraph

/// A compiled material graph.
struct MaterialProgram {
    std::vector<glm::vec4> fragment;  // two texels per instruction
    std::vector<glm::vec4> vertex;    // empty: no vertex offset
    struct Texture {
        std::string key;  // project-relative asset key
        matgraph::TextureKind kind = matgraph::TextureKind::Color;
        friend bool operator==(const Texture&, const Texture&) = default;
    };
    std::vector<Texture> textures;
    // The output node's settings.
    int alphaMode = 0;  // MaterialAsset::AlphaMode
    float alphaCutoff = 0.5f;
    bool twoSided = false;

    [[nodiscard]] std::uint32_t fragmentInstructions() const noexcept { return static_cast<std::uint32_t>(fragment.size() / 2); }
    [[nodiscard]] std::uint32_t vertexInstructions() const noexcept { return static_cast<std::uint32_t>(vertex.size() / 2); }
};

inline constexpr const char* kMaterialGraphExtension = ".lmgraph";

/// Compiles a graph document (nodes, links; as GraphEditorPanel saves it). Errors: no or
/// several output nodes, a cycle, too many instructions, registers or textures.
[[nodiscard]] Result<MaterialProgram> compileMaterialGraph(const nlohmann::json& document);
[[nodiscard]] Result<MaterialProgram> loadMaterialGraph(const std::filesystem::path& file);
/// A new graph: a Material Output with defaults (the content of a new .lmgraph).
[[nodiscard]] nlohmann::json defaultMaterialGraph();

} // namespace lumora
