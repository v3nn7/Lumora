// Motion blur (compute): each pixel averages the colour along its own motion over the frame
// (the motion vectors of the depth prepass; the sky moves with the camera alone), scaled by the
// shutter (param0) and limited to param1 pixels. Samples from surfaces behind this one count
// only where their own motion reaches this pixel, so a still background does not smear over a
// moving object's edge and a moving object does not drag the background along.
// gPush: index0 = HDR colour, index1 = depth, index2 = motion (bindless), param0 = shutter
//        (fraction of the frame), param1 = longest blur (pixels), param2 = frame number.
// Output (set 1, u0): the blurred colour.
#define LUMORA_COMPUTE 1
#include "common.hlsli"

[[vk::binding(0, 1)]] [[spv::format_rgba16f]] RWTexture2D<float4> gOut DX_REGISTER_SPACE(u0, space1);

static const int kSamples = 12;

/// Screen motion of a pixel in pixels over the last frame.
float2 pixelMotion(int2 p, float depth) {
    float2 ndcMotion;
    if (depth <= 0.0) {
        float2 uv = (float2(p) + 0.5) * gFrame.viewportSize.zw;
        float2 ndc = uv * 2.0 - 1.0;
        float4 world = mul(gFrame.inverseViewProjection, float4(ndc, 1e-6, 1.0));
        float4 previous = mul(gFrame.previousViewProjection, float4(world.xyz / world.w, 1.0));
        ndcMotion = ndc - gFrame.jitter.xy - previous.xy / max(previous.w, 1e-6);
    } else {
        ndcMotion = gTextures[gPush.index2].Load(int3(p, 0)).xy;
    }
    return ndcMotion * 0.5 * gFrame.viewportSize.xy;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const int2 size = int2(gFrame.viewportSize.xy);
    const int2 p = int2(id.xy);
    if (any(p >= size)) {
        return;
    }
    Texture2D color = gTextures[gPush.index0];
    Texture2D depthTex = gTextures[gPush.index1];
    const float4 centre = color.Load(int3(p, 0));
    const float depth = depthTex.Load(int3(p, 0)).r;
    float2 motion = pixelMotion(p, depth) * gPush.param0;
    const float length2 = dot(motion, motion);
    if (length2 < 0.25) {
        gOut[p] = centre;  // under half a pixel: nothing to blur
        return;
    }
    const float longest = gPush.param1;
    if (length2 > longest * longest) {
        motion *= longest * rsqrt(length2);
    }
    const float reach = length(motion);
    const float jitter = frac(52.9829189 * frac(dot(float2(p) + gPush.param2 * 5.588238, float2(0.06711056, 0.00583715)))) - 0.5;
    float3 sum = centre.rgb;
    float weight = 1.0;
    [unroll] for (int i = 0; i < kSamples; ++i) {
        float t = ((float(i) + 0.5 + jitter) / float(kSamples)) - 0.5;  // -0.5 .. 0.5: centred on the frame
        int2 q = clamp(p + int2(round(motion * t)), int2(0, 0), size - 1);
        float d = depthTex.Load(int3(q, 0)).r;
        float w = 1.0;
        if (d < depth - 1e-5) {
            // Behind this surface (reversed-Z: smaller is farther): only if it moves this far itself.
            float2 own = pixelMotion(q, d) * gPush.param0;
            w = saturate(length(own) - abs(t) * reach + 1.0);
        }
        sum += color.Load(int3(q, 0)).rgb * w;
        weight += w;
    }
    gOut[p] = float4(sum / weight, centre.a);
}
