// FXAA (fast approximate anti-aliasing, after Lottes' FXAA 3.11 "quality" variant): finds
// luma edges in the tonemapped image, walks along each edge to its ends and blends across it in
// proportion to where the pixel lies on the edge, plus a sub-pixel term for thin features.
//
// gPush: index0 = LDR input (gamma-encoded rgb, perceptual luma in alpha), index1 = sampler,
//        flags bit0 = output is an sRGB format (decode, the hardware encodes again).
#include "common.hlsli"
#include "fullscreen.hlsli"

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

#if defined(LUMORA_STAGE_FRAG)

static const float kEdgeThreshold = 0.125;    // minimum local contrast relative to the brightest
static const float kEdgeThresholdMin = 0.0312;  // ignore dark areas below this contrast
static const float kSubpixel = 0.75;
static const int kSearchSteps = 10;
static const float kStep[kSearchSteps] = {1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0};

float4 tap(float2 uv) {
    return gTextures[gPush.index0].SampleLevel(gSamplers[gPush.index1], uv, 0.0);
}
float lumaAt(float2 uv) { return tap(uv).a; }

float3 srgbToLinear(float3 c) {
    float3 low = c / 12.92;
    float3 high = pow(max((c + 0.055) / 1.055, 0.0.xxx), 2.4);
    return lerp(high, low, step(c, 0.04045.xxx));
}

float4 output(float3 rgb) {
    return float4((gPush.flags & 1u) != 0u ? srgbToLinear(rgb) : rgb, 1.0);
}

float4 PSMain(FullscreenOut input) : SV_Target0 {
    float2 texel = gFrame.viewportSize.zw;
    float2 uv = input.uv;
    float4 center = tap(uv);
    float lumaM = center.a;
    float lumaN = lumaAt(uv + float2(0, -1) * texel);
    float lumaS = lumaAt(uv + float2(0, 1) * texel);
    float lumaE = lumaAt(uv + float2(1, 0) * texel);
    float lumaW = lumaAt(uv + float2(-1, 0) * texel);
    float maxLuma = max(max(max(lumaN, lumaS), max(lumaE, lumaW)), lumaM);
    float minLuma = min(min(min(lumaN, lumaS), min(lumaE, lumaW)), lumaM);
    float range = maxLuma - minLuma;
    if (range < max(kEdgeThresholdMin, maxLuma * kEdgeThreshold)) {
        return output(center.rgb);  // no edge here
    }
    float lumaNW = lumaAt(uv + float2(-1, -1) * texel);
    float lumaNE = lumaAt(uv + float2(1, -1) * texel);
    float lumaSW = lumaAt(uv + float2(-1, 1) * texel);
    float lumaSE = lumaAt(uv + float2(1, 1) * texel);

    // Edge orientation from second differences.
    float horizontal = abs(lumaNW + lumaNE - 2.0 * lumaN) + 2.0 * abs(lumaW + lumaE - 2.0 * lumaM) +
                       abs(lumaSW + lumaSE - 2.0 * lumaS);
    float vertical = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaN + lumaS - 2.0 * lumaM) +
                     abs(lumaNE + lumaSE - 2.0 * lumaE);
    bool isHorizontal = horizontal >= vertical;

    // Which side of the pixel the edge lies on.
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool side1 = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));
    float stepLength = isHorizontal ? texel.y : texel.x;
    float lumaLocalAverage;
    if (side1) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaM);
    }
    float2 edgeUv = uv;
    if (isHorizontal) {
        edgeUv.y += stepLength * 0.5;
    } else {
        edgeUv.x += stepLength * 0.5;
    }

    // Walk both ways along the edge until the luma leaves the edge's average.
    float2 offset = isHorizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 uv1 = edgeUv - offset;
    float2 uv2 = edgeUv + offset;
    float lumaEnd1 = lumaAt(uv1) - lumaLocalAverage;
    float lumaEnd2 = lumaAt(uv2) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    [unroll] for (int i = 1; i < kSearchSteps; ++i) {
        if (reached1 && reached2) {
            break;
        }
        if (!reached1) {
            uv1 -= offset * kStep[i];
            lumaEnd1 = lumaAt(uv1) - lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2) {
            uv2 += offset * kStep[i];
            lumaEnd2 = lumaAt(uv2) - lumaLocalAverage;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
    }
    float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool nearer1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);
    float edgeLength = distance1 + distance2;
    float pixelOffset = -distanceFinal / edgeLength + 0.5;
    // Only blend when the nearer end agrees with the side of the pixel.
    bool lumaMSmaller = lumaM < lumaLocalAverage;
    bool correctVariation = ((nearer1 ? lumaEnd1 : lumaEnd2) < 0.0) != lumaMSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel aliasing: thin lines and single bright pixels.
    float lumaAverage = (2.0 * (lumaN + lumaS + lumaE + lumaW) + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    float subPixelOffset1 = saturate(abs(lumaAverage - lumaM) / range);
    float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    float subPixelOffset = subPixelOffset2 * subPixelOffset2 * kSubpixel;
    finalOffset = max(finalOffset, subPixelOffset);

    float2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }
    return output(tap(finalUv).rgb);
}

#endif
