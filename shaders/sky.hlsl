// Sky: the HDR environment or the procedural gradient with the sun, then a ray-marched cloud
// layer (gFrame.clouds) and volumetric fog in front of it.
// gPush: param2 = asfloat(integrated fog volume) when gFrame.fogVolume.x is set.
#include "common.hlsli"
#include "fullscreen.hlsli"

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

#if defined(LUMORA_STAGE_FRAG)
float hash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float valueNoise(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i), n100 = hash13(i + float3(1, 0, 0)), n010 = hash13(i + float3(0, 1, 0)), n110 = hash13(i + float3(1, 1, 0));
    float n001 = hash13(i + float3(0, 0, 1)), n101 = hash13(i + float3(1, 0, 1)), n011 = hash13(i + float3(0, 1, 1)), n111 = hash13(i + float3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y), lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

float fbm(float3 p, int octaves) {
    float sum = 0.0, amplitude = 0.5, total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise(p) * amplitude;
        total += amplitude;
        p = p * 2.03 + 17.1;
        amplitude *= 0.5;
    }
    return sum / total;
}

/// Extinction (per metre) of the cloud layer at a point.
float cloudDensity(float3 p, int octaves) {
    float bottom = gFrame.clouds.y;
    float thickness = max(gFrame.clouds.z, 1.0);
    float h = (p.y - bottom) / thickness;
    if (h <= 0.0 || h >= 1.0) {
        return 0.0;
    }
    float shape = saturate(h * 5.0) * saturate((1.0 - h) * 2.5);  // flat-ish bottoms, rounded tops
    float3 q = (p + float3(gFrame.cloudWind.x, 0.0, gFrame.cloudWind.y)) / 1600.0;
    float n = fbm(q, min(octaves, 3)) * shape;
    float coverage = saturate(gFrame.clouds.x);
    float d = saturate((n - (1.0 - coverage) * 0.7) / max(0.15 + coverage * 0.2, 1e-3));
    if (octaves > 2 && d > 0.0) {
        // Erode the edges with finer noise: wispy borders, solid cores.
        d = saturate(d - fbm(q * 5.0 + float3(0.0, gFrame.cameraPositionTime.w * 0.002, 0.0), 2) * (1.0 - d) * 0.8);
    }
    return d * gFrame.clouds.w * 0.012;
}

float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

/// The sky behind the clouds -> with the cloud layer in front of it.
float3 addClouds(float3 color, float3 dir, float2 pixel) {
    float3 camera = gFrame.cameraPositionTime.xyz;
    float bottom = gFrame.clouds.y;
    float top = bottom + gFrame.clouds.z;
    if (dir.y <= 0.01 || camera.y >= bottom) {
        return color;  // below the horizon, or flying above the base (not drawn from inside yet)
    }
    float t0 = (bottom - camera.y) / dir.y;
    float t1 = min((top - camera.y) / dir.y, t0 + 12000.0);
    const int steps = 28;
    float stepLength = (t1 - t0) / float(steps);
    float jitter = frac(52.9829189 * frac(dot(pixel + gFrame.cameraPositionTime.w * 61.0, float2(0.06711056, 0.00583715))));
    float3 sunDir = gFrame.sunDirection.xyz;
    float3 sun = gFrame.sunDirection.w > 0.0 ? gFrame.sunColor.rgb : 0.0;
    float cosTheta = dot(dir, sunDir);
    float phase = lerp(phaseHG(cosTheta, -0.2), phaseHG(cosTheta, 0.65), 0.7) * 4.0 * PI;
    // Light from the whole sky above (clouds are bright: they see the sky and the sun scattered
    // through themselves), darker towards their bases.
    float3 ambient = (gFrame.skyZenith.rgb + gFrame.skyHorizon.rgb) * 0.6 + sun * 0.06;
    float3 scattered = 0.0;
    float transmittance = 1.0;
    [loop] for (int i = 0; i < steps && transmittance > 0.02; ++i) {
        float3 p = camera + dir * (t0 + (float(i) + jitter) * stepLength);
        float density = cloudDensity(p, 4);
        if (density <= 0.0) {
            continue;
        }
        // Light reaching the point from the sun: optical depth along a few steps towards it.
        float toSun = 0.0;
        [unroll] for (int j = 1; j <= 3; ++j) {
            toSun += cloudDensity(p + sunDir * (float(j * j) * 50.0), 2) * float(2 * j - 1) * 50.0;
        }
        // Multiple scattering, approximated by octaves of weaker extinction (Wrenninge 2013).
        float beer = exp(-toSun) + 0.5 * exp(-toSun * 0.25) + 0.25 * exp(-toSun * 0.0625);
        float powder = 1.0 - exp(-density * 300.0);  // dark edges facing the sun
        float h = saturate((p.y - bottom) / max(gFrame.clouds.z, 1.0));
        float3 light = sun * beer * phase * lerp(1.0, powder, 0.5) * 0.3 + ambient * lerp(0.3, 1.0, h);
        float sliceTransmittance = exp(-density * stepLength);
        scattered += transmittance * light * (1.0 - sliceTransmittance);
        transmittance *= sliceTransmittance;
    }
    // Far clouds fade into the haze at the horizon.
    float fade = exp(-t0 / 25000.0);
    return lerp(color, color * transmittance + scattered, fade);
}
#endif

float4 PSMain(FullscreenOut input) : SV_Target0 {
    float3 dir = viewRay(input.uv);
    float3 color;
#if defined(LUMORA_STAGE_FRAG)
    if (environmentEnabled()) {
        color = environmentSky(dir);  // the HDR carries its own sun
    } else
#endif
    {
        color = skyGradient(dir);
        if (gFrame.sunDirection.w > 0.0) {
            float d = dot(dir, gFrame.sunDirection.xyz);
            float disk = smoothstep(0.99985, 0.99993, d);
            float glow = pow(saturate(d), 300.0) * 0.5 + pow(saturate(d), 12.0) * 0.04;
            color += gFrame.sunColor.rgb * (disk * 25.0 + glow);
        }
    }
#if defined(LUMORA_STAGE_FRAG)
    if (gFrame.clouds.x > 0.0) {
        color = addClouds(color, dir, input.position.xy);
    }
    if (gFrame.fogVolume.x != 0u) {
        float4 fog = volumetricFog(gFrame.cameraPositionTime.xyz + dir * gFrame.fogVolumeParams.x * 2.0);
        color = color * fog.a + fog.rgb;
    }
#endif
    return float4(color, 1.0);
}
