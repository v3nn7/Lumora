#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>

// CPU mirrors of the structures declared in shaders/common.hlsli.
// Layout rules: only 16-byte vectors and 4x4 matrices, so std140 == std430 == C++.

namespace lumora::render {

inline constexpr std::uint32_t kMaxLights = 16;

struct GpuLight {
    glm::vec4 positionRange{0.0f};
    glm::vec4 colorIntensity{0.0f};
    glm::vec4 directionType{0.0f};
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
    glm::uvec4 counts{0u};                  // x light count, y index of the shadowed sun (or ~0u)
    glm::mat4 lightViewProjection{1.0f};    // sun shadow map transform
    glm::vec4 shadowParams{0.0f};           // x enabled, y comparison sampler, z texel size (uv), w normal offset (m)
    glm::vec4 fogParams{0.0f};              // x density, y height falloff, z max opacity
    GpuLight lights[kMaxLights]{};
};

struct GpuObject {
    glm::mat4 world{1.0f};
    glm::mat4 normalMatrix{1.0f};
    glm::vec4 baseColorMetallic{0.0f};
    glm::vec4 emissiveRoughness{0.0f};
    glm::vec4 flags{0.0f};
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
static_assert(sizeof(GpuLight) == 48);
static_assert(offsetof(FrameConstants, cameraPositionTime) == 256);
static_assert(offsetof(FrameConstants, counts) == 384);
static_assert(offsetof(FrameConstants, lightViewProjection) == 400);
static_assert(offsetof(FrameConstants, shadowParams) == 464);
static_assert(offsetof(FrameConstants, fogParams) == 480);
static_assert(offsetof(FrameConstants, lights) == 496);
static_assert(sizeof(FrameConstants) == 496 + 48 * kMaxLights);
static_assert(sizeof(GpuObject) == 176);
static_assert(offsetof(GpuObject, flags) == 160);
static_assert(sizeof(PushConstants) == 32);

} // namespace lumora::render
