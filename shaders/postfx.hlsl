// Post effects on the HDR image before tone mapping.
//   PSDepthOfField: gPush.index0 = HDR, index1 = depth, index2 = sampler. A 24-tap disk gather
//                   whose radius follows each pixel's circle of confusion (gFrame.depthOfField).
//   PSShafts:       gPush.index0 = HDR, index1 = depth, index2 = sampler. Radial blur of the sky
//                   seen through the scene towards the sun (half resolution).
//   PSShaftsAdd:    gPush.index0 = shafts, index2 = sampler; added onto the HDR image.
#include "common.hlsli"
#include "fullscreen.hlsli"

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

#if defined(LUMORA_STAGE_FRAG)
/// Metres from the camera of a reversed infinite-Z depth (depth = near / distance).
float linearDistance(float depth) {
    return gFrame.depthOfField.w / max(depth, 1e-7);
}

float circleOfConfusion(float depth) {
    float d = linearDistance(depth);
    float outside = max(abs(d - gFrame.depthOfField.x) - gFrame.depthOfField.y * 0.5, 0.0);
    return saturate(outside / max(gFrame.depthOfField.y, 1e-3)) * gFrame.depthOfField.z;
}

static const float2 kDisk[24] = {
    float2(0.000, 0.333), float2(0.289, -0.167), float2(-0.289, -0.167), float2(0.000, -0.667),
    float2(0.577, 0.333), float2(-0.577, 0.333), float2(0.471, -0.471), float2(-0.471, -0.471),
    float2(0.667, 0.000), float2(-0.667, 0.000), float2(0.000, 0.667), float2(0.866, 0.500),
    float2(-0.866, 0.500), float2(0.000, -1.000), float2(0.500, 0.866), float2(-0.500, 0.866),
    float2(0.966, -0.259), float2(-0.966, -0.259), float2(0.707, -0.707), float2(-0.707, -0.707),
    float2(1.000, 0.000), float2(-1.000, 0.000), float2(0.259, 0.966), float2(-0.259, 0.966)};

float4 PSDepthOfField(FullscreenOut input) : SV_Target0 {
    SamplerState smp = gSamplers[gPush.index2];
    float depth = gTextures[gPush.index1].SampleLevel(smp, input.uv, 0.0).r;
    float coc = circleOfConfusion(depth);
    float3 sum = gTextures[gPush.index0].SampleLevel(smp, input.uv, 0.0).rgb;
    float weight = 1.0;
    if (coc > 0.5) {
        float2 texel = gFrame.viewportSize.zw;
        for (int i = 0; i < 24; ++i) {
            float2 uv = input.uv + kDisk[i] * coc * texel;
            float sampleDepth = gTextures[gPush.index1].SampleLevel(smp, uv, 0.0).r;
            // A sharp foreground must not bleed into a blurred background behind it.
            float w = saturate(circleOfConfusion(sampleDepth) / coc + (sampleDepth < depth ? 1.0 : 0.0));
            sum += gTextures[gPush.index0].SampleLevel(smp, uv, 0.0).rgb * w;
            weight += w;
        }
    }
    return float4(sum / weight, 1.0);
}

float4 PSShafts(FullscreenOut input) : SV_Target0 {
    SamplerState smp = gSamplers[gPush.index2];
    float2 sun = gFrame.sunShafts.xy;
    float2 step = (sun - input.uv) / 48.0;
    float2 uv = input.uv;
    float decay = 1.0;
    float3 sum = 0.0.xxx;
    for (int i = 0; i < 48; ++i) {
        uv += step;
        // Only the sky (depth at the far plane) emits; the scene blocks it.
        float sky = gTextures[gPush.index1].SampleLevel(smp, uv, 0.0).r <= 1e-7 ? 1.0 : 0.0;
        float3 c = min(gTextures[gPush.index0].SampleLevel(smp, uv, 0.0).rgb, 8.0.xxx);
        sum += c * sky * decay;
        decay *= 0.965;
    }
    return float4(sum / 48.0 * gFrame.sunShafts.z * gFrame.sunShafts.w, 1.0);
}

float4 PSShaftsAdd(FullscreenOut input) : SV_Target0 {
    return float4(gTextures[gPush.index0].SampleLevel(gSamplers[gPush.index2], input.uv, 0.0).rgb, 1.0);
}
#endif
