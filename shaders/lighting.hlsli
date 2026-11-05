// Physically based shading: Cook-Torrance GGX specular + Lambert diffuse.
#ifndef LUMORA_LIGHTING_HLSLI
#define LUMORA_LIGHTING_HLSLI

#include "common.hlsli"

float distributionGGX(float NdotH, float alpha) {
    float a2 = alpha * alpha;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

float geometrySchlickGGX(float NdotX, float k) {
    return NdotX / (NdotX * (1.0 - k) + k);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;  // direct lighting remap
    return geometrySchlickGGX(NdotV, k) * geometrySchlickGGX(NdotL, k);
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    return F0 + (max((1.0 - roughness).xxx, F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

struct Surface {
    float3 position;
    float3 normal;
    float3 view;      // towards the camera
    float3 albedo;
    float metallic;
    float roughness;
};

float3 evaluateLight(Surface s, float3 L, float3 radiance) {
    float3 H = normalize(s.view + L);
    float NdotL = saturate(dot(s.normal, L));
    float NdotV = max(dot(s.normal, s.view), 1e-4);
    float NdotH = saturate(dot(s.normal, H));
    float VdotH = saturate(dot(s.view, H));
    if (NdotL <= 0.0) {
        return 0.0.xxx;
    }
    float alpha = s.roughness * s.roughness;
    float3 F0 = lerp(0.04.xxx, s.albedo, s.metallic);
    float3 F = fresnelSchlick(VdotH, F0);
    float D = distributionGGX(NdotH, alpha);
    float G = geometrySmith(NdotV, NdotL, s.roughness);
    float3 specular = (D * G) * F / (4.0 * NdotV * NdotL + 1e-4);
    float3 kd = (1.0 - F) * (1.0 - s.metallic);
    return (kd * s.albedo / PI + specular) * radiance * NdotL;
}

/// Smooth inverse-square falloff that reaches zero at `range`.
float pointAttenuation(float distance, float range) {
    float ratio = distance / max(range, 1e-4);
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window / (distance * distance + 1.0);
}

/// Hemispherical sky/ground ambient with a roughness-blurred sky reflection. This is a
/// stand-in until image-based lighting (prefiltered environment maps) is implemented.
float3 evaluateAmbient(Surface s) {
    float NdotV = max(dot(s.normal, s.view), 1e-4);
    float3 F0 = lerp(0.04.xxx, s.albedo, s.metallic);
    float3 F = fresnelSchlickRoughness(NdotV, F0, s.roughness);
    float3 kd = (1.0 - F) * (1.0 - s.metallic);
    float hemi = s.normal.y * 0.5 + 0.5;
    float3 irradiance = lerp(gFrame.groundColor.rgb, gFrame.skyZenith.rgb, hemi) * 0.5 +
                        gFrame.skyHorizon.rgb * 0.5 * (1.0 - abs(s.normal.y));
    float3 R = reflect(-s.view, s.normal);
    float3 sharp = skyGradient(R);
    float3 blurred = lerp(gFrame.groundColor.rgb, gFrame.skyZenith.rgb, R.y * 0.5 + 0.5);
    float3 envSpecular = lerp(sharp, blurred, s.roughness);
    // Approximate split-sum environment BRDF (Karis' analytic fit).
    float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = s.roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    float2 envBrdf = float2(-1.04, 1.04) * a004 + r.zw;
    float3 specular = envSpecular * (F0 * envBrdf.x + envBrdf.y);
    return (kd * s.albedo * irradiance + specular) * gFrame.skyZenith.w;
}

#endif
