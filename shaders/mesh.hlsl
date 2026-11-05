#include "lighting.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;  // world-space tangent, w = bitangent sign
};

VSOutput VSMain(VSInput input) {
    GpuObject object = gObjects[gPush.index0];
    float4 world = mul(object.world, float4(input.position, 1.0));
    VSOutput o;
    o.position = mul(gFrame.viewProjection, world);
    o.worldPosition = world.xyz;
    o.normal = mul((float3x3)object.normalMatrix, input.normal);
    o.uv = input.uv;
    o.tangent = float4(mul((float3x3)object.world, input.tangent.xyz), input.tangent.w);
    return o;
}

/// Antialiased line mask for a world-space grid with the given cell size (1 on a line).
float gridMask(float2 coord, float cell) {
    float2 c = coord / cell;
    float2 width = max(fwidth(c), 1e-5.xx);
    float2 g = abs(frac(c - 0.5) - 0.5) / width;
    float onLine = 1.0 - saturate(min(g.x, g.y) / 1.2);
    // Fade out once cells approach pixel size to avoid moire.
    return onLine * saturate(1.0 - max(width.x, width.y) * 3.0);
}

/// Procedural prototyping patterns, projected along the dominant normal axis (world metres).
float surfacePattern(float3 p, float3 n, uint pattern) {
    if (pattern == 0u) {
        return 1.0;
    }
    float3 an = abs(n);
    float2 coord = (an.y >= an.x && an.y >= an.z) ? p.xz : (an.x >= an.z ? p.zy : p.xy);
    if (pattern == 1u) {
        float minor = gridMask(coord, 1.0);
        float major = gridMask(coord, 10.0);
        return 1.0 - 0.16 * minor - 0.28 * major;
    }
    float2 cell = floor(coord);
    float parity = abs(fmod(cell.x + cell.y, 2.0));
    return lerp(0.72, 1.0, parity);
}

/// Sun visibility from the shadow map: 3x3 hardware-filtered PCF taps (bilinear compare each),
/// normal-offset to fight acne, faded out towards the edge of the shadow region.
float sunShadow(float3 worldPosition, float3 normal) {
    if ((gPush.flags & 1u) == 0u || gFrame.shadowParams.x < 0.5) {
        return 1.0;
    }
    float3 offsetPosition = worldPosition + normal * gFrame.shadowParams.w;
    float4 lightClip = mul(gFrame.lightViewProjection, float4(offsetPosition, 1.0));
    float3 ndc = lightClip.xyz / lightClip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (any(uv < 0.0.xx) || any(uv > 1.0.xx) || ndc.z > 1.0) {
        return 1.0;
    }
    float texel = gFrame.shadowParams.z;
    uint samplerIndex = (uint)gFrame.shadowParams.y;
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            sum += gTextures[gPush.index1].SampleCmpLevelZero(gShadowSamplers[samplerIndex], uv + float2(x, y) * texel,
                                                               ndc.z - 0.0005);
        }
    }
    float visibility = sum / 9.0;
    float2 edge = abs(uv * 2.0 - 1.0);
    float fade = saturate((max(edge.x, edge.y) - 0.85) / 0.15);
    return lerp(visibility, 1.0, fade);
}

#if defined(LUMORA_STAGE_FRAG)
// Implicit-LOD sampling is fragment-only; glslang mis-compiles it when merely present in the
// vertex stage, so fragment code is compiled only for that stage.
float4 sampleMaterial(uint textureIndex, float2 uv) {
    return gTextures[textureIndex].Sample(gSamplers[gFrame.counts.z], uv);
}

float4 PSMain(VSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target0 {
    GpuObject object = gObjects[gPush.index0];
    Surface s;
    s.position = input.worldPosition;
    float faceSign = frontFace ? 1.0 : -1.0;
    float3 geometricNormal = normalize(input.normal) * faceSign;
    s.normal = geometricNormal;
    if (object.textures.w != NO_TEXTURE) {
        // Tangent-space normal map (glTF convention: +Y up in the texture, bitangent sign in w).
        float3 n = geometricNormal;
        float3 t = input.tangent.xyz - n * dot(n, input.tangent.xyz);
        if (dot(t, t) > 1e-10) {
            t = normalize(t);
            float3 b = cross(n, t) * input.tangent.w * faceSign;
            float3 m = sampleMaterial(object.textures.w, input.uv).xyz * 2.0 - 1.0;
            s.normal = normalize(t * m.x + b * m.y + n * m.z);
        }
    }
    s.view = normalize(gFrame.cameraPositionTime.xyz - input.worldPosition);
    float3 albedo = object.baseColorMetallic.rgb;
    if (object.textures.x != NO_TEXTURE) {
        albedo *= sampleMaterial(object.textures.x, input.uv).rgb;  // sRGB view: sampled in linear
    }
    s.albedo = albedo * surfacePattern(input.worldPosition, geometricNormal, (uint)object.flags.y);
    s.metallic = saturate(object.baseColorMetallic.w);
    s.roughness = object.emissiveRoughness.w;
    if (object.textures.y != NO_TEXTURE) {
        float4 mr = sampleMaterial(object.textures.y, input.uv);  // glTF: G = roughness, B = metallic
        s.roughness *= mr.g;
        s.metallic *= mr.b;
    }
    s.roughness = clamp(s.roughness, 0.02, 1.0);

    float3 color = evaluateAmbient(s);
    uint lightCount = min(gFrame.counts.x, (uint)MAX_LIGHTS);
    for (uint i = 0; i < lightCount; ++i) {
        GpuLight light = gFrame.lights[i];
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
        if ((int)light.directionType.w == LIGHT_DIRECTIONAL) {
            if (i == gFrame.counts.y) {
                radiance *= sunShadow(s.position, s.normal);
            }
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
    float3 emissive = object.emissiveRoughness.rgb;
    if (object.textures.z != NO_TEXTURE) {
        emissive *= sampleMaterial(object.textures.z, input.uv).rgb;
    }
    color += emissive;
    color = applyFog(color, s.position);

    if (object.flags.x > 0.5) {
        // Selection highlight: warm rim plus a faint tint.
        float rim = pow(1.0 - saturate(dot(s.normal, s.view)), 3.0);
        color = lerp(color, color * 0.85 + float3(1.0, 0.55, 0.1) * 0.15, 0.5) + float3(1.0, 0.45, 0.05) * rim * 2.0;
    }
    return float4(color, 1.0);
}
#endif
