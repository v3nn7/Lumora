// Volumetric fog, second pass: front to back along each froxel tile's ray (see fog.hlsl), the
// light scattered so far and the transmittance at the end of every slice: rgb = in-scattering,
// a = transmittance. applyFog (common.hlsli) then reads it at any point: colour * a + rgb.
// Input (set 1, u1): the injected volume; output (u0): the integrated one. (A separate file:
// glslang mis-compiles a compute entry point that skips the shadow lookups fog.hlsl includes.)
#define LUMORA_COMPUTE 1
#include "common.hlsli"

[[vk::binding(0, 1)]] [[spv::format_rgba16f]] RWTexture3D<float4> gOut;
[[vk::binding(1, 1)]] [[spv::format_rgba16f]] RWTexture3D<float4> gInjected;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint3 size = gFrame.fogVolume.yzw;
    if (any(id.xy >= size.xy)) {
        return;
    }
    const float far = gFrame.fogVolumeParams.x;  // slices are distances along the ray
    float3 scattered = 0.0;
    float transmittance = 1.0;
    float previous = 0.0;
    for (uint z = 0; z < size.z; ++z) {
        float end = (float(z) + 1.0) / float(size.z);
        float distance = far * end * end;
        float step = distance - previous;
        previous = distance;
        float4 froxel = gInjected[int3(id.xy, z)];
        float extinction = max(froxel.a, 1e-7);
        // Energy-conserving integration over the slice (Hillaire 2015).
        float sliceTransmittance = exp(-extinction * step);
        scattered += transmittance * (froxel.rgb - froxel.rgb * sliceTransmittance) / extinction;
        transmittance *= sliceTransmittance;
        gOut[int3(id.xy, z)] = float4(scattered, transmittance);
    }
}
