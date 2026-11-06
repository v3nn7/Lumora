// Physically based shading: GGX specular with height-correlated Smith visibility and
// multiple-scattering energy compensation, renormalised Burley diffuse, and an environment
// term integrated from the procedural sky.
#ifndef LUMORA_LIGHTING_HLSLI
#define LUMORA_LIGHTING_HLSLI

#include "common.hlsli"

struct Surface {
    float3 position;
    float3 normal;           // shading normal (normal map applied)
    float3 geometricNormal;  // interpolated vertex normal, facing the viewer
    float3 view;             // towards the camera
    float3 albedo;
    float metallic;
    float roughness;         // perceptual roughness (alpha = roughness^2)
};

float distributionGGX(float NdotH, float alpha) {
    float a2 = alpha * alpha;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

/// Height-correlated Smith masking-shadowing (Heitz 2014), already divided by 4 NdotL NdotV.
float visibilitySmithGGXCorrelated(float NdotV, float NdotL, float alpha) {
    float a2 = alpha * alpha;
    float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    return F0 + (max((1.0 - roughness).xxx, F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

/// Split-sum environment BRDF (Karis' analytic fit of the DFG integral): F0 * x + y.
float2 environmentBrdf(float NdotV, float roughness) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

/// Single-scattering GGX loses energy on rough surfaces (light that would bounce between
/// microfacets). Scaling specular by 1 + F0 (1 / Ess - 1) restores it (Fdez-Aguera 2019), so
/// rough metals keep their brightness instead of turning dark and dull.
float3 energyCompensation(float2 envBrdf, float3 F0) {
    float singleScatter = max(envBrdf.x + envBrdf.y, 1e-3);
    return 1.0 + F0 * (1.0 / singleScatter - 1.0);
}

/// Burley diffuse renormalised for energy conservation (Frostbite): rough surfaces get the
/// slight brightening at grazing angles, smooth ones darken a little there.
float diffuseBurley(float NdotV, float NdotL, float LdotH, float roughness) {
    float energyBias = lerp(0.0, 0.5, roughness);
    float energyFactor = lerp(1.0, 1.0 / 1.51, roughness);
    float f90 = energyBias + 2.0 * LdotH * LdotH * roughness;
    float lightScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NdotL, 5.0);
    float viewScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NdotV, 5.0);
    return lightScatter * viewScatter * energyFactor / PI;
}

/// Specular anti-aliasing (Kaplanyan / Tokuyoshi 2019): widens the GGX lobe by how much the
/// normal changes across the pixel, so highlights on curved or normal-mapped surfaces stop
/// sparkling and crawling. Needs the screen-space derivatives of the final normal.
float antialiasedRoughness(float roughness, float3 normalDx, float3 normalDy) {
    float variance = 0.25 * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
    float kernel = min(2.0 * variance, 0.18);
    float alpha = roughness * roughness;
    return sqrt(sqrt(saturate(alpha * alpha + kernel)));
}

/// A normal map can tilt the shading normal so that the mirror direction points into the
/// surface; that reflection is occluded by the surface itself (Jimenez 2016).
float horizonOcclusion(float3 R, float3 geometricNormal) {
    float horizon = saturate(1.0 + dot(R, geometricNormal));
    return horizon * horizon;
}

float3 evaluateLight(Surface s, float3 L, float3 radiance) {
    float NdotL = saturate(dot(s.normal, L));
    if (NdotL <= 0.0) {
        return 0.0.xxx;
    }
    float3 H = normalize(s.view + L);
    float NdotV = max(dot(s.normal, s.view), 1e-4);
    float NdotH = saturate(dot(s.normal, H));
    float LdotH = saturate(dot(L, H));
    float alpha = s.roughness * s.roughness;
    float3 F0 = lerp(0.04.xxx, s.albedo, s.metallic);
    float3 F = fresnelSchlick(LdotH, F0);
    float3 specular = distributionGGX(NdotH, alpha) * visibilitySmithGGXCorrelated(NdotV, NdotL, alpha) * F;
    specular *= energyCompensation(environmentBrdf(NdotV, s.roughness), F0);
    float3 diffuse = (1.0 - F) * (1.0 - s.metallic) * s.albedo * diffuseBurley(NdotV, NdotL, LdotH, s.roughness);
    return (diffuse + specular) * radiance * NdotL;
}

/// Smooth inverse-square falloff that reaches zero at `range`.
float pointAttenuation(float distance, float range) {
    float ratio = distance / max(range, 1e-4);
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window / (distance * distance + 1.0);
}

/// The sky averaged over a cone around `dir`: the centre plus four directions tilted by
/// `spread` (sine of the tilt). Directions below the horizon see the ground, so this also
/// carries ground bounce light.
float3 skyAverage(float3 dir, float spread) {
    float3 up = abs(dir.y) < 0.95 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 t = normalize(cross(up, dir));
    float3 b = cross(dir, t);
    float c = sqrt(saturate(1.0 - spread * spread));
    float3 sum = skyGradient(dir) * 2.0;
    sum += skyGradient(dir * c + t * spread);
    sum += skyGradient(dir * c - t * spread);
    sum += skyGradient(dir * c + b * spread);
    sum += skyGradient(dir * c - b * spread);
    return sum / 6.0;
}

/// Direction the reflection lobe mostly gathers from: rough lobes lean towards the normal
/// (Frostbite's dominant direction).
float3 dominantReflection(float3 normal, float3 view, float roughness) {
    float alpha = roughness * roughness;
    float3 R = reflect(-view, normal);
    float lean = (1.0 - alpha) * (sqrt(1.0 - alpha) + alpha);
    return normalize(lerp(normal, R, lean));
}

/// Radiance the environment (HDR map or procedural sky) reflects into a lobe of this roughness,
/// before the ambient intensity.
float3 skySpecular(float3 normal, float3 dominant, float roughness) {
#if defined(LUMORA_STAGE_FRAG) || defined(LUMORA_STAGE_COMP)
    if (environmentEnabled()) {
        return environmentRadiance(dominant, roughness * gFrame.environmentParams.w);
    }
#endif
    // Sharp lobes see the sky in one direction; wide ones approach the average around the normal.
    float alpha = roughness * roughness;
    return lerp(skyGradient(dominant), skyAverage(normal, 0.8), saturate(alpha * 1.6));
}

/// Specular radiance from the surroundings: reflection probes whose boxes hold the point
/// (smaller boxes first, each fading out over its blend distance beyond the box), the sky for
/// the rest.
/// Screen-space reflections replace it where they find the scene.
float3 environmentSpecular(float3 position, float3 normal, float3 view, float roughness) {
    float3 dominant = dominantReflection(normal, view, roughness);
    float3 sky = skySpecular(normal, dominant, roughness);
#if defined(LUMORA_STAGE_FRAG) || defined(LUMORA_STAGE_COMP)
    float3 probes = 0.0;
    float covered = 0.0;
    uint count = min(gFrame.probeCount.x, (uint)MAX_REFLECTION_PROBES);
    for (uint i = 0; i < count && covered < 0.999; ++i) {
        GpuReflectionProbe probe = gFrame.probes[i];
        // Full inside the box (surfaces on its walls too), fading out over the blend distance.
        float blend = probe.boxMin.w;
        float3 inside = min(position - probe.boxMin.xyz, probe.boxMax.xyz - position) + blend;
        float edge = min(inside.x, min(inside.y, inside.z));
        if (edge <= 0.0) {
            continue;
        }
        float weight = blend > 0.0 ? saturate(edge / blend) : 1.0;
        float3 dir = dominant;
        if (probe.position.w > 0.5) {
            // Box projection: where the reflected ray leaves the box, seen from the capture point.
            float3 far = max((probe.boxMax.xyz - position) / dominant, (probe.boxMin.xyz - position) / dominant);
            float t = min(far.x, min(far.y, far.z));
            dir = position + dominant * t - probe.position.xyz;
        }
        float lod = roughness * float(probe.textures.y);
        float3 radiance = gTextures[probe.textures.x].SampleLevel(gSamplers[gFrame.counts.z], equirectUv(normalize(dir)), lod).rgb;
        float take = weight * (1.0 - covered);
        // Baked radiance is final: undo the ambient intensity evaluateAmbient applies.
        probes += radiance * probe.boxMax.w / max(gFrame.skyZenith.w, 1e-3) * take;
        covered += take;
    }
    return probes + sky * (1.0 - covered);
#else
    return sky;
#endif
}

/// Ambient light from the environment: irradiance around the normal, specular from the
/// reflection's dominant direction. `specularWeight` is what multiplies the environment's
/// specular radiance (Fresnel, the split-sum BRDF, energy compensation, horizon occlusion).
float3 evaluateAmbient(Surface s, out float3 specularWeight) {
    float NdotV = max(dot(s.normal, s.view), 1e-4);
    float3 F0 = lerp(0.04.xxx, s.albedo, s.metallic);
    float3 F = fresnelSchlickRoughness(NdotV, F0, s.roughness);
    float2 envBrdf = environmentBrdf(NdotV, s.roughness);
    float3 R = reflect(-s.view, s.normal);

    float3 diffuseLight;
#if defined(LUMORA_STAGE_FRAG)
    if (environmentEnabled()) {
        diffuseLight = environmentIrradiance(s.normal);  // SH9
    } else
#endif
    {
        // Procedural sky. The 0.5 stands in for ambient occlusion: nothing else darkens ambient
        // light yet, and full sky light would flatten every shadow.
        diffuseLight = skyAverage(s.normal, 0.8) * 0.5;
    }
    float3 envSpecular = environmentSpecular(s.position, s.normal, s.view, s.roughness);
    float3 diffuse = (1.0 - F) * (1.0 - s.metallic) * s.albedo * diffuseLight;
    specularWeight = (F0 * envBrdf.x + envBrdf.y) * energyCompensation(envBrdf, F0) * horizonOcclusion(R, s.geometricNormal);
    return (diffuse + envSpecular * specularWeight) * gFrame.skyZenith.w;
}

float3 evaluateAmbient(Surface s) {
    float3 specularWeight;
    return evaluateAmbient(s, specularWeight);
}

#endif
