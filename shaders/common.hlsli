// Shared declarations. Must match engine/renderer/GpuTypes.hpp exactly.
#ifndef LUMORA_COMMON_HLSLI
#define LUMORA_COMMON_HLSLI

#define MAX_LIGHTS 16
#define SHADOW_CASCADES 4
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
    uint4 counts;               // x light count, y shadowed sun index (0xFFFFFFFF = none), z material sampler
    column_major float4x4 cascadeViewProjection[SHADOW_CASCADES];  // 2x2 tiles of the shadow atlas
    float4 shadowParams;        // x enabled, y comparison sampler, z 1/atlas size, w atlas size (texels)
    float4 cascadeTexelWorld;   // world size of one shadow texel per cascade (m)
    float4 shadowFade;          // x distance where shadows start fading out, y where they are gone
    float4 fogParams;           // x density, y height falloff, z max opacity
    float4 environmentParams;   // x enabled, y intensity, z rotation (radians), w max specular lod
    uint4 environmentTextures;  // x sky, y prefiltered specular (bindless), z sampler
    float4 irradianceSh[9];     // SH9 irradiance / pi
    GpuLight lights[MAX_LIGHTS];
};

struct GpuObject {
    column_major float4x4 world;
    column_major float4x4 normalMatrix;
    float4 baseColorMetallic;   // rgb base color, w metallic
    float4 emissiveRoughness;   // rgb emissive radiance, w roughness
    float4 flags;               // x selected, y surface pattern (0 none, 1 grid, 2 checker)
    uint4 textures;             // bindless: base colour, metallic-roughness, emissive, normal (NO_TEXTURE = none)
};

static const uint NO_TEXTURE = 0xFFFFFFFFu;

[[vk::binding(0, 0)]] cbuffer FrameCB { FrameData gFrame; };
[[vk::binding(1, 0)]] StructuredBuffer<GpuObject> gObjects;
[[vk::binding(0, 1)]] Texture2D gTextures[];
[[vk::binding(1, 1)]] SamplerState gSamplers[];
// Same descriptors viewed as comparison samplers (descriptor aliasing of VK_DESCRIPTOR_TYPE_SAMPLER).
[[vk::binding(1, 1)]] SamplerComparisonState gShadowSamplers[];

struct PushConstants {
    uint index0;
    uint index1;
    uint index2;
    uint flags;
    float param0;
    float param1;
    float param2;
    float param3;
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

/// Procedural sky radiance without the sun disk: zenith/horizon gradient, a bright haze band
/// at the horizon and a forward-scattering glow around the sun. Below the horizon it starts at
/// the horizon colour, so infinitely distant fogged ground meets the sky without a seam.
float3 skyGradient(float3 dir) {
    float y = dir.y;
    float3 horizon = gFrame.skyHorizon.rgb;
    float3 sky = lerp(horizon, gFrame.skyZenith.rgb, pow(saturate(y), 0.45));
    sky += horizon * 0.25 * exp(-max(y, 0.0) * 14.0);
    float3 below = lerp(horizon * 1.05, gFrame.groundColor.rgb, saturate(-y * 2.5));
    float3 color = y >= 0.0 ? sky : below;
    if (gFrame.sunDirection.w > 0.0) {
        float cosSun = saturate(dot(dir, gFrame.sunDirection.xyz));
        float3 sun = gFrame.sunColor.rgb;
        color += sun * (pow(cosSun, 6.0) * 0.035 + pow(cosSun, 48.0) * 0.08);
    }
    return color;
}

// ---- Image-based lighting (an equirectangular HDR environment) ------------------------------
// Fragment stage only: glslang mis-optimises the bindless texture array when a vertex shader
// merely contains a function that samples it.

bool environmentEnabled() { return gFrame.environmentParams.x > 0.5; }

#if defined(LUMORA_STAGE_FRAG)
/// Equirectangular uv of a world direction, turned by the environment rotation.
float2 environmentUv(float3 dir) {
    float s = sin(gFrame.environmentParams.z);
    float c = cos(gFrame.environmentParams.z);
    float3 d = float3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
    return float2(0.5 + atan2(d.x, -d.z) / (2.0 * PI), acos(clamp(d.y, -1.0, 1.0)) / PI);
}

/// Radiance of the environment in a direction; lod 0 is the sharp sky, higher lods are the
/// GGX-prefiltered levels (lod = roughness * max lod).
float3 environmentRadiance(float3 dir, float lod) {
    float2 uv = environmentUv(dir);
    uint smp = gFrame.environmentTextures.z;
    return gTextures[gFrame.environmentTextures.y].SampleLevel(gSamplers[smp], uv, lod).rgb * gFrame.environmentParams.y;
}

float3 environmentSky(float3 dir) {
    float2 uv = environmentUv(dir);
    uint smp = gFrame.environmentTextures.z;
    return gTextures[gFrame.environmentTextures.x].SampleLevel(gSamplers[smp], uv, 0.0).rgb * gFrame.environmentParams.y;
}

/// Irradiance / pi of the environment around a normal (SH9, rotated like the environment).
float3 environmentIrradiance(float3 n) {
    float s = sin(gFrame.environmentParams.z);
    float c = cos(gFrame.environmentParams.z);
    float3 d = float3(c * n.x + s * n.z, n.y, -s * n.x + c * n.z);
    float basis[9] = {0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x, 1.092548 * d.x * d.y,
                      1.092548 * d.y * d.z, 0.315392 * (3.0 * d.z * d.z - 1.0), 1.092548 * d.x * d.z,
                      0.546274 * (d.x * d.x - d.y * d.y)};
    float3 e = 0.0.xxx;
    [unroll] for (int i = 0; i < 9; ++i) {
        e += gFrame.irradianceSh[i].rgb * basis[i];
    }
    return max(e, 0.0.xxx) * gFrame.environmentParams.y;
}

#endif  // LUMORA_STAGE_FRAG

/// Exponential height fog (aerial perspective) between the camera and a world-space point,
/// coloured by the sky in that direction so distant geometry dissolves into the horizon.
float fogAmount(float3 worldPosition) {
    float density = gFrame.fogParams.x;
    if (density <= 0.0) {
        return 0.0;
    }
    float falloff = max(gFrame.fogParams.y, 1e-4);
    float3 origin = gFrame.cameraPositionTime.xyz;
    float3 delta = worldPosition - origin;
    float distance = length(delta);
    float3 dir = delta / max(distance, 1e-4);
    float k = dir.y * distance * falloff;
    // Integral of density * exp(-falloff * height) along the ray; (1 - e^-k) / k -> 1 as k -> 0.
    float shape = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
    float opticalDepth = density * exp(-origin.y * falloff) * distance * shape;
    return min(1.0 - exp(-opticalDepth), gFrame.fogParams.z);
}

float3 applyFog(float3 color, float3 worldPosition) {
    float amount = fogAmount(worldPosition);
    if (amount <= 0.0) {
        return color;
    }
    float3 dir = normalize(worldPosition - gFrame.cameraPositionTime.xyz);
    float3 fogDir = float3(dir.x, max(dir.y, 0.02), dir.z);
    // With an environment map the fog takes its blurred colour, so distance fades into the sky.
#if defined(LUMORA_STAGE_FRAG)
    float3 fogColor = environmentEnabled() ? environmentRadiance(fogDir, gFrame.environmentParams.w) : skyGradient(fogDir);
#else
    float3 fogColor = skyGradient(fogDir);
#endif
    return lerp(color, fogColor, amount);
}

#endif
