// Screen-space ambient occlusion from the depth prepass.
//
// PSOcclusion (half resolution): view-space position and normal reconstructed from depth, 12
//   samples in the normal-oriented hemisphere (radius in metres, rotated per pixel with
//   interleaved gradient noise), each compared against the depth buffer with a range check.
// PSBlur (full resolution): depth-aware 3x3 filter of the half-resolution result, so the noise
//   disappears without bleeding occlusion across silhouettes.
//
// gPush: index0 = depth (bindless), index1 = occlusion input (blur), index2 = sampler,
//        param0 = radius (m), param1 = intensity (exponent), param2 = bias (m).
#include "common.hlsli"
#include "fullscreen.hlsli"

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

#if defined(LUMORA_STAGE_FRAG)

/// Reversed infinite projection: depth = near / distance.
float viewDistance(float depth) {
    float nearPlane = gFrame.projection[2][3];  // column_major: m[3][2] in glm terms
    return nearPlane / max(depth, 1e-7);
}

float depthAt(int2 pixel) {
    int2 size = int2(gFrame.viewportSize.xy);
    pixel = clamp(pixel, int2(0, 0), size - 1);
    return gTextures[gPush.index0].Load(int3(pixel, 0)).r;
}

/// View-space position (camera looks down -Z) of a full-resolution pixel centre.
float3 viewPosition(float2 pixel, float depth) {
    float2 ndc = pixel * gFrame.viewportSize.zw * 2.0 - 1.0;
    float d = viewDistance(depth);
    return float3(ndc.x * d / gFrame.projection[0][0], ndc.y * d / gFrame.projection[1][1], -d);
}

float2 projectToPixel(float3 viewPos) {
    float d = -viewPos.z;
    float2 ndc = float2(gFrame.projection[0][0] * viewPos.x / d, gFrame.projection[1][1] * viewPos.y / d);
    return (ndc * 0.5 + 0.5) * gFrame.viewportSize.xy;
}

float noise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float PSOcclusion(FullscreenOut input) : SV_Target0 {
    float2 pixel = floor(input.uv * gFrame.viewportSize.xy) + 0.5;
    int2 ip = int2(pixel);
    float depth = depthAt(ip);
    if (depth <= 0.0) {
        return 1.0;  // sky
    }
    float3 p = viewPosition(pixel, depth);
    // Normal from the neighbour on each axis whose depth continues the surface best.
    float3 px0 = viewPosition(pixel - float2(1, 0), depthAt(ip - int2(1, 0)));
    float3 px1 = viewPosition(pixel + float2(1, 0), depthAt(ip + int2(1, 0)));
    float3 py0 = viewPosition(pixel - float2(0, 1), depthAt(ip - int2(0, 1)));
    float3 py1 = viewPosition(pixel + float2(0, 1), depthAt(ip + int2(0, 1)));
    float3 dx = abs(px1.z - p.z) < abs(p.z - px0.z) ? px1 - p : p - px0;
    float3 dy = abs(py1.z - p.z) < abs(p.z - py0.z) ? py1 - p : p - py0;
    float3 n = normalize(cross(dy, dx));
    if (dot(n, p) > 0.0) {
        n = -n;  // face the camera
    }

    float radius = gPush.param0;
    float bias = gPush.param2;
    float angle = noise(input.position.xy) * 6.2831853;
    float3 helper = abs(n.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(helper, n));
    float3 b = cross(n, t);
    float cs = cos(angle);
    float sn = sin(angle);
    float3 rt = t * cs + b * sn;
    float3 rb = -t * sn + b * cs;

    const int kSamples = 12;
    float occlusion = 0.0;
    [unroll] for (int i = 0; i < kSamples; ++i) {
        // Fibonacci spiral over the hemisphere, more samples near the centre.
        float fi = (float(i) + 0.5) / kSamples;
        float phi = float(i) * 2.39996323;
        float r = sqrt(fi);
        float3 dir = rt * (cos(phi) * r) + rb * (sin(phi) * r) + n * sqrt(1.0 - fi);
        float scale = lerp(0.15, 1.0, fi * fi);
        float3 s = p + dir * (radius * scale);
        float2 sp = projectToPixel(s);
        float sceneDistance = viewDistance(depthAt(int2(sp)));
        float sampleDistance = -s.z;
        float range = saturate(radius / max(abs(-p.z - sceneDistance), 1e-4));
        occlusion += (sceneDistance < sampleDistance - bias ? 1.0 : 0.0) * range;
    }
    float ao = 1.0 - occlusion / kSamples;
    return pow(saturate(ao), gPush.param1);
}

float PSBlur(FullscreenOut input) : SV_Target0 {
    int2 ip = int2(input.position.xy);
    float centerDistance = viewDistance(depthAt(ip));
    float2 halfTexel = gFrame.viewportSize.zw * 2.0;
    float sum = 0.0;
    float weight = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float2 uv = input.uv + float2(x, y) * halfTexel;
            float ao = gTextures[gPush.index1].SampleLevel(gSamplers[gPush.index2], uv, 0.0).r;
            float tapDistance = viewDistance(depthAt(ip + int2(x, y) * 2));
            float w = exp(-abs(tapDistance - centerDistance) / max(centerDistance * 0.02, 1e-3));
            sum += ao * w;
            weight += w;
        }
    }
    return sum / max(weight, 1e-4);
}

#endif
