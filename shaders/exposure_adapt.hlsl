// Automatic exposure, second pass: the average log2 luminance of the histogram between the
// darkest 40 % and the brightest 5 % of the lit pixels, the exposure that maps it to middle grey
// (times the scene's exposure as compensation, within [minimum, maximum]), and the eye's
// adaptation towards it.
// gPush: param0 = lowest EV, param1 = EV range, param2 = adaptation (0 keep .. 1 jump),
//        param3 = the scene's exposure (compensation), index0 = asuint(minimum exposure),
//        index1 = asuint(maximum exposure).
// Storage (set 1): u0 = the exposure (R32F 1 x 1, read and written), u1 = the histogram.
#define LUMORA_COMPUTE 1
#include "common.hlsli"

[[vk::binding(0, 1)]] [[spv::format_r32f]] RWTexture2D<float> gExposure DX_REGISTER_SPACE(u0, space1);
[[vk::binding(1, 1)]] [[spv::format_r32ui]] RWTexture2D<uint> gHistogram DX_REGISTER_SPACE(u1, space1);

groupshared float gCount[64];

[numthreads(64, 1, 1)]
void CSMain(uint index : SV_GroupIndex) {
    gCount[index] = index == 0u ? 0.0 : float(gHistogram[int2(index, 0)]);
    GroupMemoryBarrierWithGroupSync();
    if (index != 0u) {
        return;
    }
    float total = 0.0;
    for (uint i = 1u; i < 64u; ++i) {
        total += gCount[i];
    }
    float target = 1.0;
    if (total > 0.0) {
        // Weighted mean of the bins inside the percentile window.
        float low = total * 0.4, high = total * 0.95;
        float seen = 0.0, sum = 0.0, weight = 0.0;
        for (uint b = 1u; b < 64u; ++b) {
            float inBin = gCount[b];
            float take = max(min(seen + inBin, high) - max(seen, low), 0.0);
            seen += inBin;
            float ev = gPush.param0 + (float(b) - 1.0) / 62.0 * gPush.param1;
            sum += ev * take;
            weight += take;
        }
        float averageEv = weight > 0.0 ? sum / weight : 0.0;
        target = 0.18 / exp2(averageEv);
    }
    target = clamp(target * gPush.param3, asfloat(gPush.index0), asfloat(gPush.index1));
    float previous = gExposure[int2(0, 0)];
    // Adapt in log space (as the eye does): even speed from dark to bright and back.
    float exposure = previous > 0.0 && gPush.param2 < 1.0 ? exp2(lerp(log2(previous), log2(target), gPush.param2)) : target;
    gExposure[int2(0, 0)] = exposure;
}
