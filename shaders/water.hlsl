// Water: a grid displaced by four Gerstner waves, coloured by how much water the view ray passes
// through (scene depth behind the surface), with sky reflection (Fresnel), sun glints and foam
// at the shore. Alpha-blended over the opaque scene; tested against the scene depth by hand.
// gPush: index0 = object, index1 = scene depth (bindless), index2 = sampler.
// Object: baseColorMetallic.rgb shallow colour, emissiveRoughness.rgb deep colour,
//         flags = (wave height, wave length, wave speed, clarity).
#include "common.hlsli"

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
    float3 reflection = skyGradient(reflect(-v, n));
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
