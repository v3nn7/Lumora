// Physically based bloom (Jimenez 2014): 13-tap downsample chain followed by a tent-filtered
// upsample that is additively blended back up the chain.
// gPush: index0 = source texture, index1 = sampler, flags bit0 = first downsample (clamp
// fireflies), param0 = upsample filter radius in uv.
#include "common.hlsli"
#include "fullscreen.hlsli"

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

float3 tap(float2 uv) {
    return gTextures[gPush.index0].SampleLevel(gSamplers[gPush.index1], uv, 0.0).rgb;
}

float4 PSDownsample(FullscreenOut input) : SV_Target0 {
    uint w, h;
    gTextures[gPush.index0].GetDimensions(w, h);
    float2 t = 1.0 / float2(w, h);
    float2 uv = input.uv;
    float3 a = tap(uv + t * float2(-2, -2));
    float3 b = tap(uv + t * float2(0, -2));
    float3 c = tap(uv + t * float2(2, -2));
    float3 d = tap(uv + t * float2(-1, -1));
    float3 e = tap(uv + t * float2(1, -1));
    float3 f = tap(uv + t * float2(-2, 0));
    float3 g = tap(uv);
    float3 k = tap(uv + t * float2(2, 0));
    float3 i = tap(uv + t * float2(-1, 1));
    float3 j = tap(uv + t * float2(1, 1));
    float3 l = tap(uv + t * float2(-2, 2));
    float3 m = tap(uv + t * float2(0, 2));
    float3 n = tap(uv + t * float2(2, 2));
    float3 result = (d + e + i + j) * 0.125 + (a + b + f + g) * 0.03125 + (b + c + g + k) * 0.03125 +
                    (f + g + l + m) * 0.03125 + (g + k + m + n) * 0.03125;
    if ((gPush.flags & 1u) != 0u) {
        // Keep the tiny, extremely bright sun disk from flooding the frame.
        result = min(result, 24.0.xxx);
    }
    return float4(result, 1.0);
}

float4 PSUpsample(FullscreenOut input) : SV_Target0 {
    float r = gPush.param0;
    float2 uv = input.uv;
    float3 sum = tap(uv) * 4.0;
    sum += (tap(uv + float2(-r, 0)) + tap(uv + float2(r, 0)) + tap(uv + float2(0, -r)) + tap(uv + float2(0, r))) * 2.0;
    sum += tap(uv + float2(-r, -r)) + tap(uv + float2(r, -r)) + tap(uv + float2(-r, r)) + tap(uv + float2(r, r));
    return float4(sum / 16.0, 1.0);
}
