// Automatic exposure, first pass: a histogram of the scene's log2 luminance (64 bins over
// [param0, param0 + param1] EV; bin 0 = black, not counted), built per work group in shared
// memory and added into the image once per group. Every second pixel in each direction.
// gPush: index0 = HDR colour (bindless), param0 = lowest EV, param1 = EV range.
// Output (set 1, u0): R32_UINT, 64 x 1.
#define LUMORA_COMPUTE 1
#include "common.hlsli"

[[vk::binding(0, 1)]] [[spv::format_r32ui]] RWTexture2D<uint> gHistogram DX_REGISTER_SPACE(u0, space1);

groupshared uint gBins[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    gBins[index] = 0u;
    GroupMemoryBarrierWithGroupSync();
    const int2 size = int2(gFrame.viewportSize.xy);
    const int2 p = int2(id.xy) * 2;
    if (all(p < size)) {
        float3 c = gTextures[gPush.index0].Load(int3(p, 0)).rgb;
        float luminance = dot(c, float3(0.2126, 0.7152, 0.0722));
        if (luminance > 1e-5) {
            float t = saturate((log2(luminance) - gPush.param0) / gPush.param1);
            uint bin = 1u + (uint)(t * 62.0 + 0.5);
            InterlockedAdd(gBins[bin], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gBins[index] != 0u) {
        InterlockedAdd(gHistogram[int2(index, 0)], gBins[index]);
    }
}
