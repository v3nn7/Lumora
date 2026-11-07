// Shadow lookups shared by the shading passes and volumetric fog: spot and point lights (8x8
// atlas tiles), sun cascades (2x2 atlas) and the sun's virtual shadow map.
// Push constants: index1 = sun shadow map (cascade atlas or VSM pool), param0 = asfloat(local
// shadow atlas), param1 = asfloat(VSM page table), flags bit0 = sun shadows on.
#ifndef LUMORA_SHADOWS_HLSLI
#define LUMORA_SHADOWS_HLSLI

#include "common.hlsli"

// ---- Spot and point light shadows: 8x8 tiles of a second depth atlas ----------------------

/// Visibility from a shadowed spot or point light. A point light's six cube faces are six tiles
/// starting at its first one (+X, -X, +Y, -Y, +Z, -Z). 3x3 PCF from four bilinear comparisons.
float localShadow(GpuLight light, float3 position, float3 normal, float distance) {
    int tile = (int)light.spot.z;
    if (tile < 0 || gFrame.localShadowParams.x == 0.0) {
        return 1.0;
    }
    if ((int)light.directionType.w != LIGHT_SPOT) {
        float3 d = position - light.positionRange.xyz;
        float3 a = abs(d);
        tile += a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5));
    }
    // Normal offset grows with the distance (texels get larger further from the light).
    float3 p = position + normal * (0.02 + distance * 0.006);
    float4 clip = mul(gFrame.localShadowMatrices[tile], float4(p, 1.0));
    if (clip.w <= 0.0) {
        return 1.0;
    }
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z > 1.0) {
        return 1.0;
    }
    float texel = gFrame.localShadowParams.y;
    float grid = gFrame.localShadowParams.z;
    float2 tileOrigin = float2(tile % (int)grid, tile / (int)grid) / grid;
    // Stay 1.5 texels inside the tile so the filter never reads the neighbour.
    float2 inTile = clamp(ndc.xy * 0.5 + 0.5, texel * grid * 1.5, 1.0 - texel * grid * 1.5);
    float2 uv = tileOrigin + inTile / grid;
    uint atlas = asuint(gPush.param0);
    uint smp = (uint)gFrame.shadowParams.y;
    float depth = ndc.z - 0.0005;
    float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j) {
        [unroll] for (int i = 0; i < 2; ++i) {
            float2 offset = (float2(i, j) - 0.5) * texel;
            sum += gTextures[atlas].SampleCmpLevelZero(gShadowSamplers[smp], uv + offset, depth);
        }
    }
    return sum * 0.25;
}

// ---- Sun shadows: 4 cascades in a 2x2 depth atlas ---------------------------------------

/// Tile of cascade c in the 2x2 atlas (a function: glslang mis-compiles a global constant array
/// in compute shaders).
float2 cascadeTile(uint c) { return float2(c & 1u, c >> 1u); }

/// Atlas uv + depth of a world position in cascade c (tile-local uv is xy * 0.5 + 0.5).
float3 cascadeCoords(uint c, float3 worldPosition) {
    float4 clip = mul(gFrame.cascadeViewProjection[c], float4(worldPosition, 1.0));  // orthographic: w = 1
    return float3((cascadeTile(c) + clip.xy * 0.5 + 0.5) * 0.5, clip.z);
}

/// d(atlas uv, depth) for a world-space step. The cascade transform is affine, so screen-space
/// derivatives of the world position (taken in uniform control flow) map through it exactly.
float3 cascadeDelta(uint c, float3 worldDelta) {
    float3 d = mul((float3x3)gFrame.cascadeViewProjection[c], worldDelta);
    return float3(d.xy * 0.25, d.z);
}

/// Optimised 7x7 PCF (Castano 2013, as used in The Witness): 16 bilinear depth comparisons
/// whose weights add up to a smooth 7x7 tent, so penumbrae are soft and free of noise.
/// Receiver-plane depth bias keeps sloped receivers from shadowing themselves across the kernel.
float filteredShadow(float3 coords, float2 depthSlope) {
    float size = gFrame.shadowParams.w;
    float inv = gFrame.shadowParams.z;
    uint smp = (uint)gFrame.shadowParams.y;
    float2 uv = coords.xy * size;
    float2 base = floor(uv + 0.5);
    float s = uv.x + 0.5 - base.x;
    float t = uv.y + 0.5 - base.y;
    base = (base - 0.5) * inv;
    float4 uw = float4(5.0 * s - 6.0, 11.0 * s - 28.0, -(11.0 * s + 17.0), -(5.0 * s + 1.0));
    float4 u = float4((4.0 * s - 5.0) / uw.x - 3.0, (4.0 * s - 16.0) / uw.y - 1.0, -(7.0 * s + 5.0) / uw.z + 1.0,
                      -s / uw.w + 3.0);
    float4 vw = float4(5.0 * t - 6.0, 11.0 * t - 28.0, -(11.0 * t + 17.0), -(5.0 * t + 1.0));
    float4 v = float4((4.0 * t - 5.0) / vw.x - 3.0, (4.0 * t - 16.0) / vw.y - 1.0, -(7.0 * t + 5.0) / vw.z + 1.0,
                      -t / vw.w + 3.0);
    float sum = 0.0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            float2 offset = float2(u[i], v[j]) * inv;
            float bias = clamp(dot(base + offset - coords.xy, depthSlope), -0.002, 0.002);
            sum += uw[i] * vw[j] *
                   gTextures[gPush.index1].SampleCmpLevelZero(gShadowSamplers[smp], base + offset, coords.z + bias);
        }
    }
    return sum / 2704.0;
}

/// Visibility in one cascade: normal offset scaled to the cascade's texel size (more at grazing
/// angles), then the filtered comparison with a receiver-plane bias from the depth gradient.
float cascadeVisibility(uint c, float3 worldPosition, float3 normal, float ndotl, float3 dpdx, float3 dpdy) {
    float offset = gFrame.cascadeTexelWorld[c] * (0.75 + 2.0 * (1.0 - ndotl));
    float3 coords = cascadeCoords(c, worldPosition + normal * offset);
    if (coords.z >= 1.0) {
        return 1.0;
    }
    float3 dx = cascadeDelta(c, dpdx);
    float3 dy = cascadeDelta(c, dpdy);
    float det = dx.x * dy.y - dx.y * dy.x;
    float2 slope = abs(det) > 1e-12 ? float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det : 0.0.xx;
    return filteredShadow(float3(coords.xy, coords.z - 0.0001), slope);
}

/// Sun visibility. The finest cascade whose tile holds the whole filter footprint is used; near
/// its border the next cascade is blended in so the resolution change has no visible seam, and
/// past the shadow distance shadows fade out.
/// Index of the finest cascade covering a world position (SHADOW_CASCADES if none), and how
/// close it is to that cascade's border (0 centre .. 1 border).
uint selectCascade(float3 worldPosition, out float edge) {
    // Keep 8 atlas texels (filter radius + normal offset) inside the tile: in tile NDC units.
    float limit = 1.0 - 32.0 * gFrame.shadowParams.z;
    edge = 0.0;
    [loop] for (uint c = 0; c < SHADOW_CASCADES; ++c) {
        float4 clip = mul(gFrame.cascadeViewProjection[c], float4(worldPosition, 1.0));
        // Depth 0.5 is the cascade centre (see fitSunShadow), so depth also blends near its ends.
        float extent = max(max(abs(clip.x), abs(clip.y)) / limit, abs(clip.z - 0.5) * 2.0);
        if (extent < 1.0) {
            edge = extent;
            return c;
        }
    }
    return SHADOW_CASCADES;
}

// ---- Sun shadows: virtual shadow map ------------------------------------------------------
// A clipmap of levels (each twice the size of the last) split into pages; the page table holds,
// per page slot of a level, the physical page in the pool and the absolute page it holds. A page
// that is missing (not drawn yet) falls back to the next coarser level.

/// The finest level whose reach covers a view depth.
uint vsmFirstLevel(float3 worldPosition) {
    float viewDepth = -mul(gFrame.view, float4(worldPosition, 1.0)).z;
    float ratio = max(viewDepth, 1e-3) / (gFrame.vsmParams.y * gFrame.vsmParams.z);
    return (uint)clamp(ceil(log2(max(ratio, 1.0))), 0.0, gFrame.vsmParams.x - 1.0);
}

float vsmShadow(float3 worldPosition, float3 normal, float3 toLight) {
    uint levels = (uint)gFrame.vsmParams.x;
    float pages = gFrame.vsmParams.w;
    float texels = gFrame.vsmDepth.z;
    float pool = gFrame.vsmDepth.w;
    uint pageTable = asuint(gPush.param1);
    uint smp = (uint)gFrame.shadowParams.y;
    float inv = gFrame.shadowParams.z;
    float ndotl = saturate(dot(normal, toLight));
    [loop] for (uint l = vsmFirstLevel(worldPosition); l < levels; ++l) {
        float pageWorld = 2.0 * gFrame.vsmParams.y * exp2((float)l) / pages;
        float texelWorld = pageWorld / texels;
        float3 p = worldPosition + normal * texelWorld * (1.0 + 2.0 * (1.0 - ndotl));
        float3 ls = mul(gFrame.vsmLightView, float4(p, 1.0)).xyz;
        float2 pageCoords = ls.xy / pageWorld;
        float2 page = floor(pageCoords);
        int2 slot = int2(page - floor(page / pages) * pages);  // positive modulo
        float4 entry = gTextures[pageTable].Load(int3(slot.x, slot.y + (int)(l * (uint)pages), 0));
        if (entry.w < 0.5 || entry.y != page.x || entry.z != page.y) {
            continue;  // not resident (yet): a coarser level
        }
        // Stay inside the page (the filter reaches 1.5 texels out): its neighbours in the pool
        // hold other places.
        float2 inPage = clamp(pageCoords - page, 2.0 / texels, 1.0 - 2.0 / texels);
        float2 physical = float2(fmod(entry.x, pool), floor(entry.x / pool));
        float2 uv = (physical + inPage) / pool;
        float depth = (ls.z - gFrame.vsmDepth.x) * gFrame.vsmDepth.y - texelWorld * 2.0 * gFrame.vsmDepth.y;
        // 3 x 3 tent of bilinear comparisons (weights 1 2 1): a smooth 4 x 4 texel penumbra.
        float sum = 0.0;
        [unroll] for (int j = -1; j <= 1; ++j) {
            [unroll] for (int i = -1; i <= 1; ++i) {
                float weight = (i == 0 ? 2.0 : 1.0) * (j == 0 ? 2.0 : 1.0);
                sum += weight * gTextures[gPush.index1].SampleCmpLevelZero(gShadowSamplers[smp], uv + float2(i, j) * inv, depth);
            }
        }
        return sum / 16.0;
    }
    return 1.0;
}

float sunShadow(float3 worldPosition, float3 normal, float3 toLight, float3 dpdx, float3 dpdy) {
    if ((gPush.flags & 1u) == 0u || gFrame.shadowParams.x < 0.5) {
        return 1.0;
    }
    float distance = length(worldPosition - gFrame.cameraPositionTime.xyz);
    float fade = saturate((distance - gFrame.shadowFade.x) / max(gFrame.shadowFade.y - gFrame.shadowFade.x, 1e-3));
    if (fade >= 1.0) {
        return 1.0;
    }
    if (gFrame.shadowParams.x > 1.5) {
        return lerp(vsmShadow(worldPosition, normal, toLight), 1.0, fade);
    }
    float edge;
    uint first = selectCascade(worldPosition, edge);
    if (first == SHADOW_CASCADES) {
        return 1.0;
    }
    float ndotl = saturate(dot(normal, toLight));
    float visibility = cascadeVisibility(first, worldPosition, normal, ndotl, dpdx, dpdy);
    const float kBlendStart = 0.8;
    if (edge > kBlendStart) {
        float next = 1.0;  // past the last cascade: fade to lit
        if (first + 1 < SHADOW_CASCADES) {
            next = cascadeVisibility(first + 1, worldPosition, normal, ndotl, dpdx, dpdy);
        }
        visibility = lerp(visibility, next, smoothstep(kBlendStart, 1.0, edge));
    }
    return lerp(visibility, 1.0, fade);
}

#endif
