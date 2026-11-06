// Screen-space reflections (compute). For each opaque pixel the reflected ray is marched
// through the depth buffer in screen space (perspective-correct, so the steps are even in
// pixels); where it finds the scene, the lit colour there replaces the environment's specular
// that the opaque pass added. Misses, rough surfaces and rays leaving the screen fade back to
// the environment, so nothing pops.
//
//  - With temporal anti-aliasing (flags bit0) each frame reflects a different GGX direction of
//    the lobe and starts the march at a different offset; TAA turns that into glossy, smooth
//    reflections. Without it the ray is the mirror direction.
//  - The march takes 40 even steps between the surface and the screen edge (or `maxDistance`)
//    and tests the end,
//    then 6 bisection steps around the first crossing. A crossing counts when the ray is behind
//    the depth buffer by less than a thickness that grows with distance.
//
//  - Two passes. CSTrace marches one ray per pixel and stores what it saw and how much to trust
//    it. CSResolve then shares rays between neighbouring pixels of the same surface (more for
//    rougher ones, weighted against bright outliers) and puts the result into the colour.
//
// gPush: index0 = lit HDR colour, index1 = depth, index2 = normal + roughness (bindless),
//        param1 = maximum distance (m) (CSResolve: asfloat(specular weight index)),
//        param0 = CSResolve: asfloat(traced rays index), param2 = roughness where reflections
//        have faded out, param3 = frame number, flags bit0 = jitter (temporal).
// Output (set 1, u0): CSTrace: reflected radiance + confidence; CSResolve: the HDR colour with
// reflections.
#define LUMORA_COMPUTE 1
#include "lighting.hlsli"

[[vk::binding(0, 1)]] [[spv::format_rgba16f]] RWTexture2D<float4> gOut;

static const int kSteps = 40;
static const int kRefine = 6;

float interleavedGradientNoise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

/// Reverse-Z infinite projection: depth = near / view depth.
float viewDepth(float depth, float near) { return depth > 0.0 ? near / depth : 1e9; }

[numthreads(8, 8, 1)]
void CSTrace(uint3 id : SV_DispatchThreadID) {
    const int2 size = int2(gFrame.viewportSize.xy);
    const int2 p = int2(id.xy);
    if (any(p >= size)) {
        return;
    }
    Texture2D colorTex = gTextures[gPush.index0];
    Texture2D depthTex = gTextures[gPush.index1];
    Texture2D normalTex = gTextures[gPush.index2];
    const float4 color = colorTex.Load(int3(p, 0));
    const float depth = depthTex.Load(int3(p, 0)).r;
    const float4 normalRoughness = normalTex.Load(int3(p, 0));
    const float roughness = normalRoughness.w;
    const float fadeRoughness = gPush.param2;
    if (depth <= 0.0 || roughness >= fadeRoughness || dot(normalRoughness.xyz, normalRoughness.xyz) < 0.25) {
        gOut[p] = 0.0;  // sky, rough, or not an opaque surface
        return;
    }
    const float3 N = normalize(normalRoughness.xyz);
    const float2 uv = (float2(p) + 0.5) * gFrame.viewportSize.zw;
    const float2 ndc = uv * 2.0 - 1.0;  // the projection's y points down, like the image rows
    float4 w = mul(gFrame.inverseViewProjection, float4(ndc, depth, 1.0));
    const float3 position = w.xyz / w.w;
    const float3 camera = gFrame.cameraPositionTime.xyz;
    const float3 V = normalize(camera - position);

    // Direction: the mirror, or (temporal) a GGX sample of the lobe, its long tail trimmed.
    const bool jitter = (gPush.flags & 1u) != 0u;
    const float frame = gPush.param3;
    float3 R = reflect(-V, N);
    float startOffset = 0.5;
    if (jitter) {
        float2 xi = float2(interleavedGradientNoise(float2(p) + frame * 5.588238),
                           interleavedGradientNoise(float2(p.yx) + frame * 7.137921 + 13.0));
        xi.y *= 0.75;
        float alpha = roughness * roughness;
        float phi = 2.0 * PI * xi.x;
        float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
        float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
        float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        float3 t = normalize(cross(up, N));
        float3 b = cross(N, t);
        float3 H = normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + N * cosTheta);
        float3 sampled = reflect(-V, H);
        if (dot(sampled, N) > 0.0) {
            R = sampled;
        }
        startOffset = interleavedGradientNoise(float2(p) * 1.37 + frame * 3.1);
    }

    // The ray in clip space, from just off the surface to `maxDistance` (clipped at the near plane).
    const float3 origin = position + N * (0.01 + 0.002 * distance(camera, position));
    float4 c0 = mul(gFrame.viewProjection, float4(origin, 1.0));
    float4 c1 = mul(gFrame.viewProjection, float4(origin + R * gPush.param1, 1.0));
    const float near = c0.z;  // reverse-Z infinite: clip z is the near distance everywhere
    if (c1.w < near * 1.01) {
        float t = (c0.w - near * 1.01) / max(c0.w - c1.w, 1e-6);
        c1 = lerp(c0, c1, t);
    }
    const float2 s0 = c0.xy / c0.w;
    float2 s1 = c1.xy / c1.w;
    const float k0 = 1.0 / c0.w;
    float k1 = 1.0 / c1.w;
    // Stop at the screen edge: shorten the segment so its end lies inside.
    {
        float2 delta = s1 - s0;
        float scale = 1.0;
        if (s1.x > 1.0) scale = min(scale, (1.0 - s0.x) / delta.x);
        if (s1.x < -1.0) scale = min(scale, (-1.0 - s0.x) / delta.x);
        if (s1.y > 1.0) scale = min(scale, (1.0 - s0.y) / delta.y);
        if (s1.y < -1.0) scale = min(scale, (-1.0 - s0.y) / delta.y);
        scale = saturate(scale);
        s1 = s0 + delta * scale;
        k1 = lerp(k0, k1, scale);
    }

    float hitT = -1.0;
    float previousT = 0.0;
    [loop] for (int i = 0; i <= kSteps; ++i) {
        float t = min((float(i) + startOffset) / float(kSteps), 1.0);  // the last step tests the end
        float2 s = lerp(s0, s1, t);
        float rayDepth = 1.0 / lerp(k0, k1, t);
        // The segment ends at the screen edge, so only the end can land a pixel outside.
        int2 q = clamp(int2((s * 0.5 + 0.5) * gFrame.viewportSize.xy), int2(0, 0), size - 1);
        float sceneDepth = viewDepth(depthTex.Load(int3(q, 0)).r, near);
        float stepDepth = abs(rayDepth - 1.0 / lerp(k0, k1, max(t - 1.0 / float(kSteps), 0.0)));
        float thickness = 0.1 + 0.02 * sceneDepth + stepDepth;
        if (rayDepth > sceneDepth && rayDepth - sceneDepth < thickness) {
            hitT = t;
            break;
        }
        previousT = t;
    }
    float4 result = 0.0;
    if (hitT >= 0.0) {
        // Bisection between the last step in front and the first behind.
        float lo = previousT, hi = hitT;
        [unroll] for (int j = 0; j < kRefine; ++j) {
            float mid = 0.5 * (lo + hi);
            float2 s = lerp(s0, s1, mid);
            int2 q = clamp(int2((s * 0.5 + 0.5) * gFrame.viewportSize.xy), int2(0, 0), size - 1);
            float rayDepth = 1.0 / lerp(k0, k1, mid);
            if (rayDepth > viewDepth(depthTex.Load(int3(q, 0)).r, near)) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        float2 hitUv = lerp(s0, s1, hi) * 0.5 + 0.5;
        int2 hitPixel = clamp(int2(hitUv * gFrame.viewportSize.xy), int2(0, 0), size - 1);
        float3 hitNormal = normalTex.Load(int3(hitPixel, 0)).xyz;
        // Back faces of what was hit (seen from behind through the depth buffer) do not count.
        if (dot(hitNormal, R) < 0.1 || dot(hitNormal, hitNormal) < 0.25) {
            float3 reflected = colorTex.SampleLevel(gSamplers[0], hitUv, 0.0).rgb;
            float2 edge = smoothstep(0.0, 0.08, min(hitUv, 1.0 - hitUv));
            float confidence = edge.x * edge.y * (1.0 - smoothstep(0.85, 1.0, hi));
            result = float4(min(reflected, 256.0), confidence);
        }
    }
    gOut[p] = result;
}

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    const int2 size = int2(gFrame.viewportSize.xy);
    const int2 p = int2(id.xy);
    if (any(p >= size)) {
        return;
    }
    Texture2D colorTex = gTextures[gPush.index0];
    Texture2D depthTex = gTextures[gPush.index1];
    Texture2D normalTex = gTextures[gPush.index2];
    Texture2D rays = gTextures[asuint(gPush.param0)];
    const float4 color = colorTex.Load(int3(p, 0));
    const float depth = depthTex.Load(int3(p, 0)).r;
    const float4 normalRoughness = normalTex.Load(int3(p, 0));
    const float roughness = normalRoughness.w;
    const float fadeRoughness = gPush.param2;
    if (depth <= 0.0 || roughness >= fadeRoughness || dot(normalRoughness.xyz, normalRoughness.xyz) < 0.25) {
        gOut[p] = color;
        return;
    }
    const float3 N = normalize(normalRoughness.xyz);
    const float near = mul(gFrame.viewProjection, float4(gFrame.cameraPositionTime.xyz + float3(0.0, 0.0, 1.0), 1.0)).z;
    const float centreDepth = viewDepth(depth, near);

    // Neighbours' rays (a rotated ring, wider for rougher surfaces) on the same surface; each is
    // weighted by 1 / (1 + luminance) so a single bright hit does not become a speck.
    const float radius = 3.0 * saturate(roughness / 0.4);
    const float angle = (gPush.flags & 1u) != 0u ? interleavedGradientNoise(float2(p) + gPush.param3 * 2.3) * 2.0 * PI : 0.0;
    float3 radiance = 0.0;
    float radianceWeight = 0.0;
    float confidence = 0.0;
    float total = 0.0;
    [unroll] for (int i = 0; i < 9; ++i) {
        int2 q = p;
        if (i > 0) {
            float a = angle + float(i) * (2.0 * PI / 8.0);
            float r = radius * (i % 2 == 0 ? 1.0 : 0.5);
            q = clamp(p + int2(round(float2(cos(a), sin(a)) * r)), int2(0, 0), size - 1);
        }
        float4 nr = normalTex.Load(int3(q, 0));
        float d = viewDepth(depthTex.Load(int3(q, 0)).r, near);
        float w = pow(saturate(dot(N, nr.xyz)), 8.0) * saturate(1.0 - abs(d - centreDepth) / (0.05 * centreDepth + 0.05));
        if (i == 0) {
            w = 1.0;
        }
        float4 ray = rays.Load(int3(q, 0));
        float lw = w * ray.a / (1.0 + luma(ray.rgb));
        radiance += ray.rgb * lw;
        radianceWeight += lw;
        confidence += ray.a * w;
        total += w;
    }
    float3 result = color.rgb;
    if (radianceWeight > 1e-5) {
        float3 reflected = radiance / radianceWeight;
        float trust = confidence / max(total, 1e-5);
        trust *= 1.0 - smoothstep(fadeRoughness * 0.65, fadeRoughness, roughness);
        const float2 uv = (float2(p) + 0.5) * gFrame.viewportSize.zw;
        float4 w = mul(gFrame.inverseViewProjection, float4(uv * 2.0 - 1.0, depth, 1.0));
        const float3 position = w.xyz / w.w;
        const float3 V = normalize(gFrame.cameraPositionTime.xyz - position);
        float3 environment = environmentSpecular(position, N, V, roughness) * gFrame.skyZenith.w;
        float3 weight = gTextures[asuint(gPush.param1)].Load(int3(p, 0)).rgb;
        result = max(color.rgb + weight * (reflected - environment) * trust, 0.0);
    }
    gOut[p] = float4(result, color.a);
}
