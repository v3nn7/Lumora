// Screen-space ambient occlusion from the depth prepass: ground-truth ambient occlusion (GTAO,
// Jimenez et al. 2016).
//
// PSOcclusion (half resolution): view-space position and normal reconstructed from depth. In 3
//   slices around the view direction (turned per pixel with interleaved gradient noise), the
//   highest horizon on each side is found by marching the depth buffer within the radius
//   (metres); the visible part of the cosine-weighted hemisphere between the two horizons is
//   integrated exactly for the normal projected into the slice. Unlike counting samples, this
//   gives the physically right amount of darkening and no self-occlusion on flat surfaces.
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

    const float radius = gPush.param0;
    const float3 V = normalize(-p);
    // The radius on screen, in full-resolution pixels.
    const float pixelsPerMetre = abs(gFrame.projection[1][1]) * 0.5 * gFrame.viewportSize.y / max(-p.z, 1e-3);
    const float screenRadius = min(radius * pixelsPerMetre, 96.0);
    if (screenRadius < 1.5) {
        return 1.0;  // far away: smaller than a pixel
    }
    const int kSlices = 3;
    const int kSteps = 6;
    const float noiseAngle = noise(input.position.xy);
    const float noiseStep = noise(input.position.yx + 17.0);
    float visibility = 0.0;
    [unroll] for (int slice = 0; slice < kSlices; ++slice) {
        float phi = (float(slice) + noiseAngle) * (3.14159265 / float(kSlices));
        float2 omega = float2(cos(phi), sin(phi));
        // The slice's direction in view space (from the reconstruction, so screen and view axes agree).
        float3 along = viewPosition(pixel + omega, depth) - p;
        float3 direction = normalize(along - V * dot(along, V));
        float3 axis = normalize(cross(direction, V));
        float3 projected = n - axis * dot(n, axis);
        float projectedLength = length(projected);
        float cosN = saturate(dot(projected, V) / max(projectedLength, 1e-4));
        float nAngle = (dot(projected, direction) >= 0.0 ? 1.0 : -1.0) * acos(cosN);  // towards +direction: positive
        // Highest horizon on each side (cosine against the view direction).
        float horizonCos[2] = {-1.0, -1.0};
        [unroll] for (int side = 0; side < 2; ++side) {
            float sign = side == 0 ? -1.0 : 1.0;
            [unroll] for (int step = 0; step < kSteps; ++step) {
                float t = (float(step) + noiseStep) / float(kSteps);
                float2 offset = omega * sign * max(t * t * screenRadius, 1.0 + float(step));
                float2 samplePixel = pixel + offset;
                float3 s = viewPosition(samplePixel, depthAt(int2(samplePixel)));
                float3 delta = s - p;
                float lengthSq = dot(delta, delta);
                float cosH = dot(delta, V) * rsqrt(max(lengthSq, 1e-8));
                // Far samples fade back towards the lowest horizon (no occlusion from across the room).
                float falloff = saturate(1.0 - lengthSq / (radius * radius));
                horizonCos[side] = max(horizonCos[side], lerp(-1.0, cosH, falloff));
            }
        }
        float h0 = -acos(clamp(horizonCos[0], -1.0, 1.0));
        float h1 = acos(clamp(horizonCos[1], -1.0, 1.0));
        h0 = nAngle + max(h0 - nAngle, -1.5707963);
        h1 = nAngle + min(h1 - nAngle, 1.5707963);
        float sinN = sin(nAngle);
        float arc0 = -cos(2.0 * h0 - nAngle) + cosN + 2.0 * h0 * sinN;
        float arc1 = -cos(2.0 * h1 - nAngle) + cosN + 2.0 * h1 * sinN;
        visibility += projectedLength * 0.25 * (arc0 + arc1);
    }
    float ao = visibility / float(kSlices);
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
