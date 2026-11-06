// Diagnostic compute pass: writes a known pattern into storage image u0 (set 1), so tests can
// check compute dispatch, storage images and their hand-off to later passes.
// gPush: param0 = blue, index0/index1 = image size.
#define LUMORA_COMPUTE 1
#include "common.hlsli"

// Storage images name their format with glslang's [[spv::format_*]] (rgba8, rgba16f, r32f...);
// without it glslc assumes rgba32f.
[[vk::binding(0, 1)]] [[spv::format_rgba8]] RWTexture2D<float4> gOut;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gPush.index0 || id.y >= gPush.index1) {
        return;
    }
    gOut[id.xy] = float4(float(id.x) / float(gPush.index0), float(id.y) / float(gPush.index1), gPush.param0, 1.0);
}
