// Water: a grid displaced by four Gerstner waves, coloured by how much water the view ray passes
// through (scene depth behind the surface), with reflections (Fresnel), sun glints and foam
// at the shore. Alpha-blended over the opaque scene; tested against the scene depth by hand.
// Reflections: the scene on screen where a short screen-space march finds it (when the frame
// has screen-space reflections), else reflection probes and the sky.
// gPush: index0 = object, index1 = scene depth (bindless), index2 = sampler,
//        param0 = asfloat(scene colour before water, bindless), flags bit0 = it is there.
// Object: baseColorMetallic.rgb shallow colour, emissiveRoughness.rgb deep colour,
//         flags = (wave height, wave length, wave speed, clarity).
#include "lighting.hlsli"

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    nointerpolation uint objectIndex : TEXCOORD2;
};

static const float2 kWaveDirections[4] = {float2(1.0, 0.0), float2(0.6, 0.8), float2(-0.45, 0.9), float2(0.8, -0.6)};
static const float kWaveLengths[4] = {1.0, 0.61, 1.37, 0.43};
static const float kWaveAmplitudes[4] = {0.5, 0.25, 0.35, 0.12};

VSOutput VSMain([[vk::location(0)]] float3 position : POSITION, uint instance : SV_InstanceID) {
    const uint objectIndex = gPush.index0 + instance;
    GpuObject object = gObjects[objectIndex];
    float3 world = mul(object.world, float4(position, 1.0)).xyz;
    const float height = object.flags.x, length = max(object.flags.y, 0.1), speed = object.flags.z;
    const float time = gFrame.cameraPositionTime.w;
    float3 displaced = world;
    float3 normal = float3(0.0, 1.0, 0.0);
    [unroll] for (int i = 0; i < 4; ++i) {
        const float k = 2.0 * PI / (length * kWaveLengths[i]);
        const float a = height * kWaveAmplitudes[i];
        const float c = sqrt(9.81 / k) * speed;
        const float2 d = kWaveDirections[i];
        const float f = k * (dot(d, world.xz) - c * time);
        const float q = 0.6 / max(k * a * 4.0, 1.0);  // steepness, kept below crests looping over
        displaced.xz += q * a * d * cos(f);
        displaced.y += a * sin(f);
        normal.xz -= d * k * a * cos(f);
        normal.y -= q * k * a * sin(f);
    }
    VSOutput o;
    o.position = toTarget(mul(gFrame.viewProjection, float4(displaced, 1.0)));
    o.worldPosition = displaced;
    o.normal = normal;
    o.objectIndex = objectIndex;
    return o;
}

#if defined(LUMORA_STAGE_FRAG)
/// The scene colour the reflected ray meets on screen (rgb) and how much to trust it (a).
float4 screenReflection(float3 origin, float3 R, Texture2D depthTex, Texture2D colorTex, float2 pixel) {
    float4 c0 = mul(gFrame.viewProjection, float4(origin, 1.0));
    float4 c1 = mul(gFrame.viewProjection, float4(origin + R * 80.0, 1.0));
    const float near = c0.z;  // reverse-Z infinite: clip z is the near distance
    if (c1.w < near * 1.01) {
        c1 = lerp(c0, c1, (c0.w - near * 1.01) / max(c0.w - c1.w, 1e-6));
    }
    float2 s0 = c0.xy / c0.w, s1 = c1.xy / c1.w;
    float k0 = 1.0 / c0.w, k1 = 1.0 / c1.w;
    float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    const int steps = 32;
    float previous = 0.0;
    [loop] for (int i = 0; i < steps; ++i) {
        float t = (float(i) + jitter) / float(steps);
        t *= t;  // finer near the surface
        float2 uv = lerp(s0, s1, t) * 0.5 + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0)) {
            break;
        }
        float rayDepth = 1.0 / lerp(k0, k1, t);
        float d = depthTex.SampleLevel(gSamplers[gPush.index2], uv, 0.0).r;
        float sceneDepth = d > 0.0 ? near / d : 1e9;
        float stepDepth = abs(rayDepth - 1.0 / lerp(k0, k1, previous));
        if (rayDepth > sceneDepth && rayDepth - sceneDepth < 0.2 + 0.03 * sceneDepth + stepDepth) {
            float2 edge = smoothstep(0.0, 0.08, min(uv, 1.0 - uv));
            float3 color = colorTex.SampleLevel(gSamplers[gPush.index2], uv, 0.0).rgb;
            return float4(min(color, 256.0), edge.x * edge.y * (1.0 - smoothstep(0.7, 1.0, t)));
        }
        previous = t;
    }
    return 0.0;
}

float4 PSMain(VSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target0 {
    GpuObject object = gObjects[input.objectIndex];
    float2 uv = input.position.xy * gFrame.viewportSize.zw;
    float sceneDepth = gTextures[gPush.index1].SampleLevel(gSamplers[gPush.index2], uv, 0.0).r;
    if (input.position.z < sceneDepth) {
        discard;  // behind something opaque (reversed-Z: larger is nearer)
    }
    float thickness = 1e4;
    if (sceneDepth > 0.0) {
        float4 behind = mul(gFrame.inverseViewProjection, float4(uv * 2.0 - 1.0, sceneDepth, 1.0));
        thickness = length(behind.xyz / behind.w - input.worldPosition);
    }
    float3 n = normalize(input.normal) * (frontFace ? 1.0 : -1.0);
    float3 v = normalize(gFrame.cameraPositionTime.xyz - input.worldPosition);
    const float clarity = max(object.flags.w, 0.05);
    float absorbed = 1.0 - exp(-thickness / clarity);
    float3 water = lerp(object.baseColorMetallic.rgb, object.emissiveRoughness.rgb, absorbed);
    float3 sky = skyGradient(float3(0.0, 1.0, 0.0));
    float3 lit = water * (sky * gFrame.skyZenith.w + 0.15);
    float3 l = gFrame.sunDirection.xyz;
    if (gFrame.sunDirection.w > 0.0) {
        lit += water * gFrame.sunColor.rgb * saturate(dot(n, l)) * 0.25;
    }
    float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(n, v)), 5.0);
    float3 reflection = environmentSpecular(input.worldPosition, n, v, 0.04) * gFrame.skyZenith.w;
    if ((gPush.flags & 1u) != 0u) {
        float4 seen = screenReflection(input.worldPosition + n * 0.02, reflect(-v, n), gTextures[gPush.index1],
                                       gTextures[asuint(gPush.param0)], input.position.xy);
        reflection = lerp(reflection, seen.rgb, seen.a);
    }
    float3 color = lerp(lit, reflection, fresnel);
    if (gFrame.sunDirection.w > 0.0) {
        float3 h = normalize(l + v);
        color += gFrame.sunColor.rgb * pow(saturate(dot(n, h)), 400.0) * 3.0;
    }
    float foam = 1.0 - saturate(thickness / 0.3);
    color = lerp(color, (sky + gFrame.sunColor.rgb * 0.3) * 0.9, foam * 0.7);
    float alpha = saturate(max(lerp(0.2, 1.0, absorbed), max(fresnel, foam * 0.8)));
    return float4(applyFog(color, input.worldPosition), alpha);
}
#endif
