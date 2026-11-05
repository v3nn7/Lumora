// Shared declarations. Must match engine/renderer/GpuTypes.hpp exactly.
#ifndef LUMORA_COMMON_HLSLI
#define LUMORA_COMMON_HLSLI

#define MAX_LIGHTS 16
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1

static const float PI = 3.14159265359;

struct GpuLight {
    float4 positionRange;   // xyz world position, w range
    float4 colorIntensity;  // rgb color, w intensity
    float4 directionType;   // xyz direction the light travels, w type
};

struct FrameData {
    column_major float4x4 view;
    column_major float4x4 projection;
    column_major float4x4 viewProjection;
    column_major float4x4 inverseViewProjection;
    float4 cameraPositionTime;  // xyz camera, w seconds
    float4 viewportSize;        // w, h, 1/w, 1/h
    float4 skyZenith;           // rgb, w ambient intensity
    float4 skyHorizon;          // rgb, w exposure
    float4 groundColor;         // rgb
    float4 sunDirection;        // xyz towards the sun, w = 1 if a sun exists
    float4 sunColor;            // rgb radiance
    float4 gridParams;          // x enabled, y cell size, z fade distance
    uint4 counts;               // x light count, y shadowed sun index (0xFFFFFFFF = none)
    column_major float4x4 lightViewProjection;
    float4 shadowParams;        // x enabled, y comparison sampler, z texel size (uv), w normal offset
    GpuLight lights[MAX_LIGHTS];
};

struct GpuObject {
    column_major float4x4 world;
    column_major float4x4 normalMatrix;
    float4 baseColorMetallic;   // rgb base color, w metallic
    float4 emissiveRoughness;   // rgb emissive radiance, w roughness
    float4 flags;               // x selected
};

[[vk::binding(0, 0)]] cbuffer FrameCB { FrameData gFrame; };
[[vk::binding(1, 0)]] StructuredBuffer<GpuObject> gObjects;
[[vk::binding(0, 1)]] Texture2D gTextures[];
[[vk::binding(1, 1)]] SamplerState gSamplers[];
// Same descriptors viewed as comparison samplers (descriptor aliasing of VK_DESCRIPTOR_TYPE_SAMPLER).
[[vk::binding(1, 1)]] SamplerComparisonState gShadowSamplers[];

struct PushConstants {
    uint index0;
    uint index1;
    float param0;
    uint flags;
};
// Note: glslang only honours vk::push_constant on ConstantBuffer<T>/cbuffer declarations; a
// plain struct global silently lands in the $Global uniform block instead.
[[vk::push_constant]] ConstantBuffer<PushConstants> gPush;

/// World-space direction through a screen uv (0..1, top-left origin).
float3 viewRay(float2 uv) {
    float2 ndc = uv * 2.0 - 1.0;
    float4 nearPoint = mul(gFrame.inverseViewProjection, float4(ndc, 1.0, 1.0));  // reversed-Z: near = 1
    return normalize(nearPoint.xyz / nearPoint.w - gFrame.cameraPositionTime.xyz);
}

/// Procedural sky radiance (no sun disk). Also used as a crude environment for ambient light.
float3 skyGradient(float3 dir) {
    float y = dir.y;
    float3 sky = lerp(gFrame.skyHorizon.rgb, gFrame.skyZenith.rgb, pow(saturate(y), 0.4));
    float3 ground = lerp(gFrame.skyHorizon.rgb * 0.6, gFrame.groundColor.rgb, saturate(-y * 6.0));
    return y >= 0.0 ? sky : ground;
}

#endif
