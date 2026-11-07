// Shared declarations. Must match engine/renderer/GpuTypes.hpp exactly.
#ifndef LUMORA_COMMON_HLSLI
#define LUMORA_COMMON_HLSLI

#define MAX_LIGHTS 16
#define SHADOW_CASCADES 4
#define LOCAL_SHADOW_TILES 64
#define MAX_REFLECTION_PROBES 8
#define MAX_PROBE_VOLUMES 4
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

static const float PI = 3.14159265359;

struct GpuLight {
    float4 positionRange;   // xyz world position, w range
    float4 colorIntensity;  // rgb color, w intensity
    float4 directionType;   // xyz direction the light travels, w type
    float4 spot;            // x cos(outer angle), y cos(inner angle), z first shadow tile (-1 = none)
};

struct GpuReflectionProbe {
    float4 boxMin;    // w blend distance (m)
    float4 boxMax;    // w intensity
    float4 position;  // where it was captured; w 1 = box projection
    uint4 textures;   // x prefiltered specular (bindless), y its highest mip
};

struct GpuProbeVolume {
    float4 boxMin;  // w blend distance (m)
    float4 boxMax;  // w intensity
    uint4 grid;     // x 3D texture (bindless: 7 texels of SH9 per probe, stacked along z), yzw probes along x, y, z
    float4 params;  // x normal bias (m)
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
    float4 shadowParams;        // x 1 cascades / 2 virtual, y comparison sampler, z 1/atlas size, w atlas size (texels)
    float4 cascadeTexelWorld;   // world size of one shadow texel per cascade (m)
    float4 shadowFade;          // x distance where shadows start fading out, y where they are gone
    float4 fogParams;           // x density, y height falloff, z max opacity
    float4 environmentParams;   // x enabled, y intensity, z rotation (radians), w max specular lod
    uint4 environmentTextures;  // x sky, y prefiltered specular (bindless), z sampler
    float4 irradianceSh[9];     // SH9 irradiance / pi
    uint4 lightGrid;            // x tiles across, y tiles down, z tile size (px), w point lights
    column_major float4x4 localShadowMatrices[LOCAL_SHADOW_TILES];  // spot / point-face tiles (8x8 atlas)
    float4 localShadowParams;   // x enabled, y 1/atlas size, z tiles per side
    float4 grading;             // x contrast, y saturation, z temperature, w tint
    float4 depthOfField;        // x focus distance, y focus range, z max blur (px), w near plane
    float4 sunShafts;           // xy sun on screen (uv), z intensity, w visibility
    column_major float4x4 vsmLightView;  // virtual shadow map: world -> light space (m)
    float4 vsmParams;           // x levels, y half size of level 0 (m), z level reach, w pages per level
    float4 vsmDepth;            // x depth min, y depth scale, z page texels, w pool pages per side
    column_major float4x4 previousViewProjection;  // last frame's, without jitter (motion vectors)
    float4 jitter;              // xy this frame's sub-pixel offset (NDC), zw last frame's
    uint4 probeCount;           // x reflection probes
    GpuReflectionProbe probes[MAX_REFLECTION_PROBES];
    uint4 volumeCount;          // x light probe volumes
    GpuProbeVolume volumes[MAX_PROBE_VOLUMES];
    uint4 fogVolume;            // x 1 = volumetric fog, yzw froxels along x, y, z
    float4 fogVolumeParams;     // x distance it reaches (m), y anisotropy, z depth jitter (0..1)
    float4 fogAlbedo;           // rgb: share of the light the fog scatters
    float4 clouds;              // x coverage, y bottom (m), z thickness (m), w density
    float4 cloudWind;           // xy how far the wind moved the clouds (m)
    GpuLight lights[MAX_LIGHTS];  // directional lights
};

struct GpuObject {
    column_major float4x4 world;
    column_major float4x4 normalMatrix;
    float4 baseColorMetallic;   // rgb base color, w metallic
    float4 emissiveRoughness;   // rgb emissive radiance, w roughness
    float4 flags;               // x selected, y surface pattern (0 none, 1 grid, 2 checker)
    uint4 textures;             // bindless: base colour, metallic-roughness, emissive, normal (NO_TEXTURE = none)
    float4 surface;             // x alpha cutoff (0 = opaque; below it the base colour's alpha cuts a hole), y two-sided,
                                // z occlusion map (asuint, NO_TEXTURE = none), w normal map strength
    column_major float4x4 previousWorld;  // last frame's world matrix (motion vectors)
};

static const uint NO_TEXTURE = 0xFFFFFFFFu;

// D3D12 registers, only for DXIL (glslang turns a register on the push constants into a binding).
#if defined(LUMORA_DXIL)
#define DX_REGISTER(r) : register(r)
#define DX_REGISTER_SPACE(r, space) : register(r, space)
#else
#define DX_REGISTER(r)
#define DX_REGISTER_SPACE(r, space)
#endif

// Every resource names its Vulkan binding ([[vk::binding(binding, set)]], SPIR-V) and its D3D12
// register (register(...), DXIL); the C++ binding layouts match both (RenderContext.cpp).
[[vk::binding(0, 0)]] cbuffer FrameCB DX_REGISTER(b0) { FrameData gFrame; };
[[vk::binding(1, 0)]] StructuredBuffer<GpuObject> gObjects DX_REGISTER(t1);
// Point lights, binned into screen tiles on the CPU: gLightGrid holds (first, count) per tile,
// then the light indices those ranges point into.
[[vk::binding(3, 0)]] StructuredBuffer<GpuLight> gPointLights DX_REGISTER(t3);
[[vk::binding(4, 0)]] StructuredBuffer<uint> gLightGrid DX_REGISTER(t4);

// Skinning palettes: GpuObject.flags.z is where an object's joint matrices start.
struct GpuJoint {
    column_major float4x4 transform;
};
[[vk::binding(2, 0)]] StructuredBuffer<GpuJoint> gJoints DX_REGISTER(t2);

/// Blend of up to four joint matrices (skinned meshes).
float4x4 skinMatrix(uint4 joints, float4 weights, uint first) {
    return gJoints[first + joints.x].transform * weights.x + gJoints[first + joints.y].transform * weights.y +
           gJoints[first + joints.z].transform * weights.z + gJoints[first + joints.w].transform * weights.w;
}

// Set 1 in graphics pipelines; compute shaders define LUMORA_COMPUTE first, which moves it to set
// 2 (NVRHI puts bindless layouts after the regular sets, and compute has its storage images in 1).
// The bindless table also holds 3D textures (light probe volumes), read through gVolumes.
// D3D12: the table is t0 in space 1 for 2D views and again in space 2 for 3D ones (registers do
// not alias), in graphics and compute alike.
#if defined(LUMORA_COMPUTE)
[[vk::binding(0, 2)]] Texture2D gTextures[] DX_REGISTER_SPACE(t0, space1);
[[vk::binding(0, 2)]] Texture3D gVolumes[] DX_REGISTER_SPACE(t0, space2);
#else
[[vk::binding(0, 1)]] Texture2D gTextures[] DX_REGISTER_SPACE(t0, space1);
[[vk::binding(0, 1)]] Texture3D gVolumes[] DX_REGISTER_SPACE(t0, space2);
#endif
[[vk::binding(5, 0)]] SamplerState gSamplers[4] DX_REGISTER(s5);
// Same descriptors viewed as comparison samplers (Vulkan: aliasing VK_DESCRIPTOR_TYPE_SAMPLER;
// D3D12: the same four samplers bound again at s9).
[[vk::binding(5, 0)]] SamplerComparisonState gShadowSamplers[4] DX_REGISTER(s9);

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
[[vk::push_constant]] ConstantBuffer<PushConstants> gPush DX_REGISTER(b1);  // D3D12: root constants

// Clip-space position for the rasterizer. Lumora's matrices use Vulkan's clip space (+Y down);
// the GPU layer (NVRHI) presents D3D's (+Y up) on every API, flipping Vulkan's viewport, so
// vertex shaders flip Y back. The image comes out the same on Vulkan and D3D12.
float4 toTarget(float4 clip) {
    clip.y = -clip.y;
    return clip;
}

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

#if defined(LUMORA_STAGE_FRAG) || defined(LUMORA_STAGE_COMP)
/// Equirectangular uv of a world direction, turned by the environment rotation.
float2 environmentUv(float3 dir) {
    float s = sin(gFrame.environmentParams.z);
    float c = cos(gFrame.environmentParams.z);
    float3 d = float3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
    return float2(0.5 + atan2(d.x, -d.z) / (2.0 * PI), acos(clamp(d.y, -1.0, 1.0)) / PI);
}

/// Equirectangular uv of a world direction (baked reflection probes: not rotated).
float2 equirectUv(float3 d) {
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

/// Light probe volumes: irradiance / pi around the normal (rgb, final radiance units) from the
/// volumes holding the point, and how much of it they cover (a; the sky's ambient for the rest).
/// Each volume fades out over its blend distance beyond its box; earlier volumes (smaller) win.
float4 probeVolumeIrradiance(float3 position, float3 n) {
    float3 sum = 0.0;
    float covered = 0.0;
    uint count = min(gFrame.volumeCount.x, (uint)MAX_PROBE_VOLUMES);
    for (uint i = 0; i < count && covered < 0.999; ++i) {
        GpuProbeVolume volume = gFrame.volumes[i];
        float blend = volume.boxMin.w;
        float3 inside = min(position - volume.boxMin.xyz, volume.boxMax.xyz - position) + blend;
        float edge = min(inside.x, min(inside.y, inside.z));
        if (edge <= 0.0) {
            continue;
        }
        float weight = blend > 0.0 ? saturate(edge / blend) : 1.0;
        // Off the surface along the normal, so a wall does not read the probes behind it.
        float3 uvw = saturate((position + n * volume.params.x - volume.boxMin.xyz) / (volume.boxMax.xyz - volume.boxMin.xyz));
        float3 counts = float3(volume.grid.yzw);
        float2 xy = clamp(uvw.xy, 0.5 / counts.xy, 1.0 - 0.5 / counts.xy);
        float z = clamp(uvw.z * counts.z, 0.5, counts.z - 0.5);  // stays inside one coefficient's block
        float values[28];
        [unroll] for (uint t = 0; t < 7; ++t) {
            float4 texel = gVolumes[volume.grid.x].SampleLevel(gSamplers[0], float3(xy, (float(t) * counts.z + z) / (7.0 * counts.z)), 0.0);
            values[t * 4] = texel.x;
            values[t * 4 + 1] = texel.y;
            values[t * 4 + 2] = texel.z;
            values[t * 4 + 3] = texel.w;
        }
        float basis[9] = {0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y,
                          1.092548 * n.y * n.z, 0.315392 * (3.0 * n.z * n.z - 1.0), 1.092548 * n.x * n.z,
                          0.546274 * (n.x * n.x - n.y * n.y)};
        float3 irradiance = 0.0;
        [unroll] for (uint k = 0; k < 9; ++k) {
            irradiance += float3(values[k * 3], values[k * 3 + 1], values[k * 3 + 2]) * basis[k];
        }
        float take = weight * (1.0 - covered);
        sum += max(irradiance, 0.0) * volume.boxMax.w * take;
        covered += take;
    }
    return float4(sum, covered);
}

#endif  // LUMORA_STAGE_FRAG || LUMORA_STAGE_COMP

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

/// Analytic height fog: the colour it fades into along a direction.
float3 fogColorTowards(float3 dir) {
    float3 fogDir = float3(dir.x, max(dir.y, 0.02), dir.z);
    // With an environment map the fog takes its blurred colour, so distance fades into the sky.
#if defined(LUMORA_STAGE_FRAG)
    return environmentEnabled() ? environmentRadiance(fogDir, gFrame.environmentParams.w) : skyGradient(fogDir);
#else
    return skyGradient(fogDir);
#endif
}

#if defined(LUMORA_STAGE_FRAG)
/// Volumetric fog between the camera and a point (rgb in-scattering, a transmittance), from the
/// integrated froxel volume (bindless index in gPush.param2); see fog.hlsl.
float4 volumetricFog(float3 worldPosition) {
    float distance = length(worldPosition - gFrame.cameraPositionTime.xyz);
    float4 clip = mul(gFrame.viewProjection, float4(worldPosition, 1.0));
    float2 uv = clip.xy / max(clip.w, 1e-6) * 0.5 + 0.5;
    float slices = float(gFrame.fogVolume.w);
    // Texel z holds the end of slice z; slice ends lie at far * ((z + 1) / slices)^2.
    float boundary = sqrt(saturate(distance / gFrame.fogVolumeParams.x)) * slices;
    float4 fog = gVolumes[asuint(gPush.param2)].SampleLevel(gSamplers[0], float3(uv, (boundary - 0.5) / slices), 0.0);
    return lerp(float4(0.0, 0.0, 0.0, 1.0), fog, saturate(boundary));  // nothing in front of the first slice end
}
#endif

float3 applyFog(float3 color, float3 worldPosition) {
#if defined(LUMORA_STAGE_FRAG)
    if (gFrame.fogVolume.x != 0u) {
        // Beyond the volume the analytic fog covers the rest of the way, then the volume on top.
        float3 delta = worldPosition - gFrame.cameraPositionTime.xyz;
        float far = gFrame.fogVolumeParams.x;
        float distance = length(delta);
        if (distance > far) {
            float3 dir = delta / distance;
            float total = 1.0 - fogAmount(worldPosition);
            float near = 1.0 - fogAmount(gFrame.cameraPositionTime.xyz + dir * far);
            float beyond = saturate(total / max(near, 1e-4));
            color = lerp(fogColorTowards(dir), color, beyond);
        }
        float4 fog = volumetricFog(worldPosition);
        return color * fog.a + fog.rgb;
    }
#endif
    float amount = fogAmount(worldPosition);
    if (amount <= 0.0) {
        return color;
    }
    float3 dir = normalize(worldPosition - gFrame.cameraPositionTime.xyz);
    return lerp(color, fogColorTowards(dir), amount);
}

#endif
