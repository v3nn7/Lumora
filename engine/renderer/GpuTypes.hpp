#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <bit>
#include <cstdint>

// CPU mirrors of the structures declared in shaders/common.hlsli.
// Layout rules: only 16-byte vectors and 4x4 matrices, so std140 == std430 == C++.

namespace lumora::render {

inline constexpr std::uint32_t kMaxLights = 16;
/// Shadow atlas tiles for spot (one each) and point lights (six each, a cube's faces).
inline constexpr std::uint32_t kLocalShadowTiles = 16;          // directional lights in FrameConstants
inline constexpr std::uint32_t kMaxPointLights = 4096;    // point lights (storage buffer, tiled)
inline constexpr std::uint32_t kLightTileSize = 32;       // pixels per light-grid tile side
/// Sun shadow cascades, rendered as the 2x2 tiles of one depth atlas.
inline constexpr std::uint32_t kShadowCascades = 4;

struct GpuLight {
    glm::vec4 positionRange{0.0f};
    glm::vec4 colorIntensity{0.0f};
    glm::vec4 directionType{0.0f};
    glm::vec4 spot{-1.0f, -1.0f, -1.0f, 0.0f};  // x cos(outer), y cos(inner), z first shadow tile (-1 none)
};

struct FrameConstants {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 viewProjection{1.0f};
    glm::mat4 inverseViewProjection{1.0f};
    glm::vec4 cameraPositionTime{0.0f};
    glm::vec4 viewportSize{0.0f};
    glm::vec4 skyZenith{0.0f};
    glm::vec4 skyHorizon{0.0f};
    glm::vec4 groundColor{0.0f};
    glm::vec4 sunDirection{0.0f};
    glm::vec4 sunColor{0.0f};
    glm::vec4 gridParams{0.0f};
    glm::uvec4 counts{0u};                  // x light count, y shadowed sun (or ~0u), z material sampler
    glm::mat4 cascadeViewProjection[kShadowCascades]{};  // world -> cascade clip (orthographic)
    glm::vec4 shadowParams{0.0f};           // x 1 cascades / 2 virtual, y comparison sampler, z 1/atlas size, w atlas size (texels)
    glm::vec4 cascadeTexelWorld{0.0f};      // world size of one shadow texel in each cascade (m)
    glm::vec4 shadowFade{0.0f};             // x distance where shadows start to fade, y where they end
    glm::vec4 fogParams{0.0f};              // x density, y height falloff, z max opacity
    glm::vec4 environmentParams{0.0f};      // x enabled, y intensity, z rotation (radians), w max specular lod
    glm::uvec4 environmentTextures{0u};     // x sky, y prefiltered specular (bindless), z sampler
    glm::vec4 irradianceSh[9]{};            // SH9 irradiance / pi (rgb)
    glm::uvec4 lightGrid{0u};               // x tiles across, y tiles down, z tile size (px), w point lights
    glm::mat4 localShadowMatrices[kLocalShadowTiles]{};  // spot / point-face view-projections (atlas tiles)
    glm::vec4 localShadowParams{0.0f};      // x enabled, y 1/atlas size (texels)
    glm::vec4 grading{1.0f, 1.0f, 0.0f, 0.0f};  // x contrast, y saturation, z temperature, w tint
    glm::vec4 depthOfField{0.0f};           // x focus distance, y focus range, z max blur (px), w near plane
    glm::vec4 sunShafts{0.0f};              // xy sun on screen (uv), z intensity, w visibility
    glm::mat4 vsmLightView{1.0f};           // virtual shadow map: world -> light space (m)
    glm::vec4 vsmParams{0.0f};              // x levels, y half size of level 0 (m), z level reach, w pages per level
    glm::vec4 vsmDepth{0.0f};               // x depth min, y depth scale, z page texels, w pool pages per side
    glm::mat4 previousViewProjection{1.0f}; // last frame's, without jitter (motion vectors)
    glm::vec4 jitter{0.0f};                 // xy this frame's sub-pixel offset (NDC), zw last frame's
    GpuLight lights[kMaxLights]{};
};

inline constexpr std::uint32_t kNoTexture = 0xffffffffu;

/// One corner of a particle quad (see particle.hlsl).
struct ParticleVertex {
    glm::vec3 position;
    glm::vec2 corner;  // [-1, 1]
    glm::vec4 color;   // linear HDR rgb + alpha
};
static_assert(sizeof(ParticleVertex) == 36);

struct GpuObject {
    glm::mat4 world{1.0f};
    glm::mat4 normalMatrix{1.0f};
    glm::vec4 baseColorMetallic{0.0f};
    glm::vec4 emissiveRoughness{0.0f};
    glm::vec4 flags{0.0f};
    glm::uvec4 textures{kNoTexture};  // bindless: base colour, metallic-roughness, emissive, normal
    glm::vec4 surface{0.0f, 0.0f, std::bit_cast<float>(kNoTexture), 1.0f};  // x alpha cutoff (0 = opaque), y two-sided, z occlusion map, w normal strength
    glm::mat4 previousWorld{1.0f};    // last frame's world matrix (motion vectors)
};

struct PushConstants {
    std::uint32_t index0 = 0;
    std::uint32_t index1 = 0;
    std::uint32_t index2 = 0;
    std::uint32_t flags = 0;
    float param0 = 0.0f;
    float param1 = 0.0f;
    float param2 = 0.0f;
    float param3 = 0.0f;
};

// Offsets verified against the SPIR-V produced from common.hlsli.
static_assert(sizeof(GpuLight) == 64);
static_assert(offsetof(FrameConstants, cameraPositionTime) == 256);
static_assert(offsetof(FrameConstants, counts) == 384);
static_assert(offsetof(FrameConstants, cascadeViewProjection) == 400);
static_assert(offsetof(FrameConstants, shadowParams) == 656);
static_assert(offsetof(FrameConstants, cascadeTexelWorld) == 672);
static_assert(offsetof(FrameConstants, shadowFade) == 688);
static_assert(offsetof(FrameConstants, fogParams) == 704);
static_assert(offsetof(FrameConstants, localShadowMatrices) == 912);
static_assert(offsetof(FrameConstants, vsmLightView) == 912 + 64 * kLocalShadowTiles + 64);
static_assert(offsetof(FrameConstants, previousViewProjection) == 912 + 64 * kLocalShadowTiles + 64 + 96);
static_assert(offsetof(FrameConstants, lights) == 912 + 64 * kLocalShadowTiles + 64 + 96 + 80);
static_assert(sizeof(FrameConstants) == 912 + 64 * kLocalShadowTiles + 64 + 96 + 80 + 64 * kMaxLights);
static_assert(sizeof(GpuObject) == 272);
static_assert(offsetof(GpuObject, previousWorld) == 208);
static_assert(offsetof(GpuObject, surface) == 192);
static_assert(offsetof(GpuObject, textures) == 176);
static_assert(offsetof(GpuObject, flags) == 160);
static_assert(sizeof(PushConstants) == 32);

} // namespace lumora::render
