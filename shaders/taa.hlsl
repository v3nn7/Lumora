// Temporal anti-aliasing (compute). Each frame is rendered with a sub-pixel jitter; this pass
// blends it into a history that follows the scene through the motion vectors, so edges, thin
// geometry and cut-out leaves converge to a supersampled image instead of crawling.
//
//  - Reprojection: motion of the nearest surface in the 3x3 neighbourhood (silhouettes keep the
//    foreground's motion); the sky reprojects with the camera alone.
//  - History rejection: the history is clipped to the variance box of the current 3x3 colours in
//    YCoCg, so what was revealed or changed does not leave ghosts.
//  - Blending in a luminance-weighted space (Karis), which stops bright pixels from flickering.
//  - The history is read with a 5-tap Catmull-Rom filter, which keeps it sharp under motion.
//
// gPush: index0 = current HDR colour, index1 = depth, index2 = motion (bindless),
//        param0 = asfloat(history index), param1 = weight of the current frame,
//        flags bit0 = the history is valid.
// Output (set 1, u0): the new history, which the frame's post-processing then reads.
#define LUMORA_COMPUTE 1
#include "common.hlsli"

[[vk::binding(0, 1)]] [[spv::format_rgba16f]] RWTexture2D<float4> gHistoryOut;

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 weightIn(float3 c) { return c / (1.0 + luma(c)); }     // HDR -> bounded
float3 weightOut(float3 c) { return c / max(1.0 - luma(c), 1e-4); }
float3 toYCoCg(float3 c) {
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}
float3 fromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

/// Catmull-Rom history sample from 5 bilinear taps (the corners are dropped).
float3 sampleHistory(uint history, float2 uv, float2 size) {
    SamplerState smp = gSamplers[0];  // linear clamp
    float2 position = uv * size;
    float2 center = floor(position - 0.5) + 0.5;
    float2 f = position - center;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc0 = (center - 1.0) / size;
    float2 tc3 = (center + 2.0) / size;
    float2 tc12 = (center + w2 / w12) / size;
    Texture2D t = gTextures[history];
    float3 result = t.SampleLevel(smp, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y) +
                    t.SampleLevel(smp, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y) +
                    t.SampleLevel(smp, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y) +
                    t.SampleLevel(smp, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y) +
                    t.SampleLevel(smp, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(result / weight, 0.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const int2 size = int2(gFrame.viewportSize.xy);
    const int2 p = int2(id.xy);
    if (any(p >= size)) {
        return;
    }
    Texture2D current = gTextures[gPush.index0];
    Texture2D depth = gTextures[gPush.index1];
    Texture2D motion = gTextures[gPush.index2];

    // Neighbourhood statistics (YCoCg of the weighted colour) and the nearest surface.
    float3 m1 = 0.0, m2 = 0.0;
    float3 centre = 0.0;
    float nearest = -1.0;
    int2 nearestAt = p;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 q = clamp(p + int2(x, y), int2(0, 0), size - 1);
            float3 c = toYCoCg(weightIn(current.Load(int3(q, 0)).rgb));
            m1 += c;
            m2 += c * c;
            if (x == 0 && y == 0) {
                centre = c;
            }
            float d = depth.Load(int3(q, 0)).r;  // reversed-Z: larger is nearer
            if (d > nearest) {
                nearest = d;
                nearestAt = q;
            }
        }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    // Where this pixel was last frame.
    float2 uv = (float2(p) + 0.5) * gFrame.viewportSize.zw;
    float2 velocity;
    if (nearest <= 0.0) {
        // Sky: only the camera moved.
        float2 ndc = uv * 2.0 - 1.0;
        float4 world = mul(gFrame.inverseViewProjection, float4(ndc + gFrame.jitter.xy, 1e-6, 1.0));
        float4 previous = mul(gFrame.previousViewProjection, float4(world.xyz / world.w, 1.0));
        velocity = ndc - previous.xy / max(previous.w, 1e-6);
    } else {
        velocity = motion.Load(int3(nearestAt, 0)).xy;
    }
    float2 previousUv = uv - velocity * 0.5;

    float3 result = centre;
    bool valid = (gPush.flags & 1u) != 0u && all(previousUv > 0.0) && all(previousUv < 1.0);
    if (valid) {
        float3 history = toYCoCg(weightIn(sampleHistory(asuint(gPush.param0), previousUv, gFrame.viewportSize.xy)));
        // Variance clipping: towards the box centre until the history fits.
        float3 extent = sigma * 1.25 + 1e-4;
        float3 offset = history - mean;
        float3 scaled = abs(offset / extent);
        float outside = max(scaled.x, max(scaled.y, scaled.z));
        if (outside > 1.0) {
            history = mean + offset / outside;
        }
        // Fast motion trusts the current frame more (the history is blurred by resampling).
        float speed = length(velocity * gFrame.viewportSize.xy * 0.5);
        float weight = lerp(gPush.param1, 0.35, saturate(speed / 32.0));
        result = lerp(history, centre, weight);
    }
    gHistoryOut[p] = float4(weightOut(fromYCoCg(result)), 1.0);
}
