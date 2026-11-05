#include "lighting.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VSOutput VSMain(VSInput input) {
    GpuObject object = gObjects[gPush.index0];
    float4 world = mul(object.world, float4(input.position, 1.0));
    VSOutput o;
    o.position = mul(gFrame.viewProjection, world);
    o.worldPosition = world.xyz;
    o.normal = mul((float3x3)object.normalMatrix, input.normal);
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target0 {
    GpuObject object = gObjects[gPush.index0];
    Surface s;
    s.position = input.worldPosition;
    s.normal = normalize(input.normal) * (frontFace ? 1.0 : -1.0);
    s.view = normalize(gFrame.cameraPositionTime.xyz - input.worldPosition);
    s.albedo = object.baseColorMetallic.rgb;
    s.metallic = saturate(object.baseColorMetallic.w);
    s.roughness = clamp(object.emissiveRoughness.w, 0.02, 1.0);

    float3 color = evaluateAmbient(s);
    uint lightCount = min(gFrame.counts.x, (uint)MAX_LIGHTS);
    for (uint i = 0; i < lightCount; ++i) {
        GpuLight light = gFrame.lights[i];
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
        if ((int)light.directionType.w == LIGHT_DIRECTIONAL) {
            color += evaluateLight(s, -light.directionType.xyz, radiance);
        } else {
            float3 toLight = light.positionRange.xyz - s.position;
            float distance = length(toLight);
            float attenuation = pointAttenuation(distance, light.positionRange.w);
            if (attenuation > 0.0) {
                color += evaluateLight(s, toLight / max(distance, 1e-4), radiance * attenuation);
            }
        }
    }
    color += object.emissiveRoughness.rgb;

    if (object.flags.x > 0.5) {
        // Selection highlight: warm rim plus a faint tint.
        float rim = pow(1.0 - saturate(dot(s.normal, s.view)), 3.0);
        color = lerp(color, color * 0.85 + float3(1.0, 0.55, 0.1) * 0.15, 0.5) + float3(1.0, 0.45, 0.05) * rim * 2.0;
    }
    return float4(color, 1.0);
}
