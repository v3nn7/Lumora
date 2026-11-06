#include "common.hlsli"
#include "fullscreen.hlsli"

// gPush: index0 = HDR scene, index1 = sampler, index2 = bloom texture, flags bit0 = encode sRGB,
//        bit1 = bloom present, bit2 = perceptual luma into alpha (input of FXAA),
//        bit3 = linear output: the scene's HDR radiance unchanged (probe baking);
//        param0 = exposure, param1 = bloom strength, param2 = vignette,
//        param3 = bloom normalisation (1 / number of summed chain levels).

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

/// ACES filmic approximation (Narkowicz 2015).
float3 acesFilm(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 linearToSrgb(float3 c) {
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(max(c, 0.0.xxx), 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(c, 0.0031308.xxx));
}

/// Interleaved gradient noise, used to dither away 8-bit banding.
float gradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float4 PSMain(FullscreenOut input) : SV_Target0 {
    float3 hdr = gTextures[gPush.index0].SampleLevel(gSamplers[gPush.index1], input.uv, 0.0).rgb;
    if ((gPush.flags & 8u) != 0u) {
        return float4(hdr, 1.0);
    }
    if ((gPush.flags & 2u) != 0u) {
        float3 bloom = gTextures[gPush.index2].SampleLevel(gSamplers[gPush.index1], input.uv, 0.0).rgb;
        hdr = lerp(hdr, bloom * gPush.param3, gPush.param1);
    }
    float2 centered = input.uv - 0.5;
    float vignette = saturate(1.0 - gPush.param2 * pow(dot(centered, centered) * 2.2, 1.3));
    // White balance in linear light: warm adds red and takes blue, tint trades green for magenta.
    float temperature = gFrame.grading.z;
    float tint = gFrame.grading.w;
    hdr *= float3(1.0 + 0.2 * temperature + 0.1 * tint, 1.0 - 0.2 * tint, 1.0 - 0.2 * temperature + 0.1 * tint);
    float3 color = acesFilm(hdr * gPush.param0 * vignette);
    // Contrast around middle grey, then saturation, on the display-referred image.
    color = saturate((color - 0.18) * gFrame.grading.x + 0.18);
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = saturate(lerp(luma.xxx, color, gFrame.grading.y));
    if ((gPush.flags & 1u) != 0u) {
        color = linearToSrgb(color);
    }
    float noise = (gradientNoise(input.position.xy) - 0.5) / 255.0;
    color += noise;
    // FXAA reads luma from alpha: of the gamma-encoded colour, like the eye sees contrast.
    float alpha = (gPush.flags & 4u) != 0u ? dot(saturate(color), float3(0.299, 0.587, 0.114)) : 1.0;
    return float4(color, alpha);
}
