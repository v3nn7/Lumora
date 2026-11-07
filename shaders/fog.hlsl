// Volumetric fog in froxels: a grid aligned with the view (8 x 8 pixel tiles, 64 slices out to
// fogVolumeParams.x, slices denser near the camera: distance = far * (slice / 64)^2).
//
//  - CSInject: per froxel, the fog's density (exponential height fog), and the light it scatters
//    towards the camera: the sun with its shadows (so shafts appear where light gets through),
//    point and spot lights binned in the froxel's screen tile, and ambient light from the sky.
//    Writes rgb = scattered radiance * scattering coefficient, a = extinction.
//  - fog_integrate.hlsl then integrates it along each tile's ray.
//
// gPush: index1 = sun shadow map, param0 = asfloat(local shadow atlas),
//                 param1 = asfloat(VSM page table), flags bit0 = sun shadows.
// Output (set 1, u0): the injected volume.
#define LUMORA_COMPUTE 1
#include "lighting.hlsli"
#include "shadows.hlsli"

[[vk::binding(0, 1)]] [[spv::format_rgba16f]] RWTexture3D<float4> gOut DX_REGISTER_SPACE(u0, space1);

/// Henyey-Greenstein phase function (normalised over the sphere).
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

float fogDensityAt(float3 p) {
    return gFrame.fogParams.x * exp(-max(gFrame.fogParams.y, 1e-4) * p.y);
}

[numthreads(4, 4, 4)]
void CSInject(uint3 id : SV_DispatchThreadID) {
    const uint3 size = gFrame.fogVolume.yzw;
    if (any(id >= size)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(size.xy);
    const float3 camera = gFrame.cameraPositionTime.xyz;
    float4 farPoint = mul(gFrame.inverseViewProjection, float4(uv * 2.0 - 1.0, 0.5, 1.0));
    const float3 dir = normalize(farPoint.xyz / farPoint.w - camera);
    // The froxel's centre (jittered along the ray between frames when TAA blends them).
    const float slice = (float(id.z) + gFrame.fogVolumeParams.z) / float(size.z);
    const float distance = gFrame.fogVolumeParams.x * slice * slice;
    const float3 p = camera + dir * distance;

    const float extinction = fogDensityAt(p);
    if (extinction <= 0.0) {
        gOut[id] = 0.0;
        return;
    }
    const float g = gFrame.fogVolumeParams.y;
    float3 light = 0.0;
    uint directional = min(gFrame.counts.x, (uint)MAX_LIGHTS);
    for (uint i = 0; i < directional; ++i) {
        GpuLight sun = gFrame.lights[i];
        float3 toLight = -sun.directionType.xyz;
        float3 radiance = sun.colorIntensity.rgb * sun.colorIntensity.w;
        if (i == gFrame.counts.y) {
            radiance *= sunShadow(p, toLight, toLight, 0.0, 0.0);
        }
        light += radiance * phaseHG(dot(dir, toLight), g);
    }
    if (gFrame.lightGrid.w > 0u) {
        uint2 pixel = uint2(uv * gFrame.viewportSize.xy);
        uint2 tile = min(pixel / gFrame.lightGrid.z, gFrame.lightGrid.xy - 1u);
        uint t = tile.y * gFrame.lightGrid.x + tile.x;
        uint first = gLightGrid[t * 2u];
        uint count = gLightGrid[t * 2u + 1u];
        for (uint k = 0; k < count; ++k) {
            GpuLight l = gPointLights[gLightGrid[first + k]];
            float3 toLight = l.positionRange.xyz - p;
            float d = length(toLight);
            float attenuation = pointAttenuation(d, l.positionRange.w);
            if ((int)l.directionType.w == LIGHT_SPOT) {
                attenuation *= smoothstep(l.spot.x, l.spot.y, dot(-toLight / max(d, 1e-4), l.directionType.xyz));
            }
            if (attenuation > 0.0) {
                float3 L = toLight / max(d, 1e-4);
                float3 radiance = l.colorIntensity.rgb * l.colorIntensity.w * attenuation * localShadow(l, p, L, d);
                light += radiance * phaseHG(dot(dir, L), g);
            }
        }
    }
    // Ambient: the sky around, scattered evenly; baked light probe volumes indoors.
    float3 ambient;
    if (environmentEnabled()) {
        ambient = 0.5 * (environmentIrradiance(float3(0.0, 1.0, 0.0)) + environmentIrradiance(float3(0.0, -1.0, 0.0)));
    } else {
        ambient = skyAverage(float3(0.0, 1.0, 0.0), 1.0) * 0.5;
    }
    ambient *= gFrame.skyZenith.w;
    float4 baked = probeVolumeIrradiance(p, float3(0.0, 1.0, 0.0));
    ambient = lerp(ambient, baked.rgb, baked.a);
    light += ambient;  // irradiance / pi is the average radiance around; the phase integrates to 1
    gOut[id] = float4(light * extinction * gFrame.fogAlbedo.rgb, extinction);
}
