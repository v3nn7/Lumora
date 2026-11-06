#include "lighting.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
#if defined(LUMORA_SKINNED)
    [[vk::location(4)]] uint4 joints : BLENDINDICES;
    [[vk::location(5)]] float4 weights : BLENDWEIGHT;
#endif
};

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;  // world-space tangent, w = bitangent sign
    nointerpolation uint objectIndex : TEXCOORD4;
};

// Instanced draws: copies of one mesh are consecutive objects starting at gPush.index0.
VSOutput VSMain(VSInput input, uint instance : SV_InstanceID) {
    const uint objectIndex = gPush.index0 + instance;
    GpuObject object = gObjects[objectIndex];
    float3 position = input.position;
    float3 normal = input.normal;
    float3 tangent = input.tangent.xyz;
#if defined(LUMORA_SKINNED)
    // Joint matrices bring the vertex from bind pose into the mesh node's space.
    float4x4 skin = skinMatrix(input.joints, input.weights, (uint)object.flags.z);
    position = mul(skin, float4(position, 1.0)).xyz;
    normal = mul((float3x3)skin, normal);
    tangent = mul((float3x3)skin, tangent);
#endif
    float4 world = mul(object.world, float4(position, 1.0));
    VSOutput o;
    o.position = toTarget(mul(gFrame.viewProjection, world));
    o.worldPosition = world.xyz;
    o.normal = mul((float3x3)object.normalMatrix, normal);
    o.uv = input.uv;
    o.tangent = float4(mul((float3x3)object.world, tangent), input.tangent.w);
    o.objectIndex = objectIndex;
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
    if (pattern == 0u || pattern == 3u) {
        return 1.0;  // (3 = terrain: coloured in terrainAlbedo)
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

float terrainHash(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float terrainNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(terrainHash(i), terrainHash(i + float2(1, 0)), f.x),
                lerp(terrainHash(i + float2(0, 1)), terrainHash(i + float2(1, 1)), f.x), f.y);
}

/// Terrain: grass on flat ground, rock (packed in flags.z as sqrt-encoded RGB8) on slopes, with
/// a little large-scale variation so wide fields do not look flat.
float3 terrainAlbedo(float3 grass, float packedRock, float3 position, float3 normal) {
    uint packed = (uint)packedRock;
    float3 rock = float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u) / 255.0;
    rock *= rock;
    float variation = terrainNoise(position.xz * 0.08) * 0.6 + terrainNoise(position.xz * 0.5) * 0.4;
    float slope = 1.0 - saturate(normal.y);
    float rocky = smoothstep(0.22, 0.38, slope + (variation - 0.5) * 0.12);
    float3 g = grass * lerp(0.82, 1.15, variation);
    return lerp(g, rock * lerp(0.85, 1.1, variation), rocky);
}

#include "shadows.hlsli"

#if defined(LUMORA_STAGE_FRAG)
// Implicit-LOD sampling is fragment-only; glslang mis-compiles it when merely present in the
// vertex stage, so fragment code is compiled only for that stage.
float4 sampleMaterial(uint textureIndex, float2 uv) {
    return gTextures[textureIndex].Sample(gSamplers[gFrame.counts.z], uv);
}

#if defined(LUMORA_REFLECTIONS)
// The opaque pass for screen-space reflections also writes what they need per pixel.
struct PSOutput {
    float4 color : SV_Target0;
    float4 normalRoughness : SV_Target1;  // world shading normal, perceptual roughness
    float4 specular : SV_Target2;         // weight of the environment's specular (see evaluateAmbient)
};
PSOutput PSMain(VSOutput input, bool frontFace : SV_IsFrontFace) {
#else
float4 PSMain(VSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target0 {
#endif
    GpuObject object = gObjects[input.objectIndex];
    // Screen-space derivatives must be taken in uniform control flow (used by shadow filtering).
    float3 dpdx = ddx(input.worldPosition);
    float3 dpdy = ddy(input.worldPosition);
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
            m.xy *= object.surface.w;  // glTF normalTexture.scale
            s.normal = normalize(t * m.x + b * m.y + n * m.z);
        }
    }
    s.geometricNormal = geometricNormal;
    s.view = normalize(gFrame.cameraPositionTime.xyz - input.worldPosition);
    float3 albedo = object.baseColorMetallic.rgb;
    if ((uint)object.flags.y == 3u) {
        albedo = terrainAlbedo(albedo, object.flags.z, input.worldPosition, geometricNormal);
    }
    // See-through (surface.y bit 1): surface.x is the opacity, which the map's alpha scales.
    const bool seeThrough = ((uint)object.surface.y & 2u) != 0u;
    float alpha = seeThrough ? object.surface.x : 1.0;
    if (object.textures.x != NO_TEXTURE) {
        float4 base = sampleMaterial(object.textures.x, input.uv);  // sRGB view: sampled in linear
        albedo *= base.rgb;
        if (seeThrough) {
            alpha *= base.a;
        } else if (base.a < object.surface.x) {
            discard;  // cut out (leaves, fences): the depth prepass cut the same hole
        }
    }
    s.albedo = albedo * surfacePattern(input.worldPosition, geometricNormal, (uint)object.flags.y);
    s.metallic = saturate(object.baseColorMetallic.w);
    s.roughness = object.emissiveRoughness.w;
    if (object.textures.y != NO_TEXTURE) {
        float4 mr = sampleMaterial(object.textures.y, input.uv);  // glTF: G = roughness, B = metallic
        s.roughness *= mr.g;
        s.metallic *= mr.b;
    }
    // Derivatives in uniform control flow: the normal-map branch above depends only on the draw.
    s.roughness = antialiasedRoughness(clamp(s.roughness, 0.02, 1.0), ddx(s.normal), ddy(s.normal));

    float3 specularWeight;
    float3 color = evaluateAmbient(s, specularWeight);
    float occlusion = 1.0;
    if (asuint(object.surface.z) != NO_TEXTURE) {
        occlusion *= sampleMaterial(asuint(object.surface.z), input.uv).r;  // baked ambient occlusion (glTF: red)
    }
    if ((gPush.flags & 4u) != 0u) {
        // Screen-space ambient occlusion (index2) darkens light from the sky in creases.
        occlusion *= gTextures[gPush.index2].Load(int3(int2(input.position.xy), 0)).r;
    }
    color *= occlusion;
    uint lightCount = min(gFrame.counts.x, (uint)MAX_LIGHTS);
    for (uint i = 0; i < lightCount; ++i) {  // directional lights
        GpuLight light = gFrame.lights[i];
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
        if (i == gFrame.counts.y) {
            radiance *= sunShadow(s.position, geometricNormal, -light.directionType.xyz, dpdx, dpdy);
        }
        color += evaluateLight(s, -light.directionType.xyz, radiance);
    }
    if (gFrame.lightGrid.w > 0u) {
        // Point lights binned into this pixel's screen tile.
        uint2 tile = min(uint2(input.position.xy) / gFrame.lightGrid.z, gFrame.lightGrid.xy - 1u);
        uint t = tile.y * gFrame.lightGrid.x + tile.x;
        uint first = gLightGrid[t * 2u];
        uint count = gLightGrid[t * 2u + 1u];
        for (uint k = 0; k < count; ++k) {
            GpuLight light = gPointLights[gLightGrid[first + k]];
            float3 toLight = light.positionRange.xyz - s.position;
            float distance = length(toLight);
            float attenuation = pointAttenuation(distance, light.positionRange.w);
            if ((int)light.directionType.w == LIGHT_SPOT) {
                // Cone: full inside the inner angle, fading to nothing at the outer one.
                float cosAngle = dot(-toLight / max(distance, 1e-4), light.directionType.xyz);
                attenuation *= smoothstep(light.spot.x, light.spot.y, cosAngle);
            }
            if (attenuation > 0.0) {
                float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
                radiance *= localShadow(light, s.position, geometricNormal, distance);
                color += evaluateLight(s, toLight / max(distance, 1e-4), radiance);
            }
        }
    }
    float3 emissive = object.emissiveRoughness.rgb;
    if (object.textures.z != NO_TEXTURE) {
        emissive *= sampleMaterial(object.textures.z, input.uv).rgb;
    }
    color += emissive;
    if ((gPush.flags & 2u) != 0u && gFrame.counts.y != 0xFFFFFFFFu) {
        // Debug view: red, green, blue, yellow = cascade 0..3, darker towards the blend band.
        static const float3 kCascadeTint[SHADOW_CASCADES] = {float3(1.0, 0.25, 0.25), float3(0.25, 1.0, 0.25),
                                                             float3(0.3, 0.45, 1.0), float3(1.0, 1.0, 0.25)};
        float edge;
        uint cascade = gFrame.shadowParams.x > 1.5 ? vsmFirstLevel(s.position) % SHADOW_CASCADES : selectCascade(s.position, edge);
        if (cascade < SHADOW_CASCADES) {
            color *= kCascadeTint[cascade] * (edge > 0.8 ? 0.6 : 1.0);
        }
    }
    color = applyFog(color, s.position);

    if (object.flags.x > 0.5) {
        // Selection highlight: warm rim plus a faint tint.
        float rim = pow(1.0 - saturate(dot(s.normal, s.view)), 3.0);
        color = lerp(color, color * 0.85 + float3(1.0, 0.55, 0.1) * 0.15, 0.5) + float3(1.0, 0.45, 0.05) * rim * 2.0;
    }
#if defined(LUMORA_REFLECTIONS)
    PSOutput o;
    o.color = float4(color, 1.0);
    o.normalRoughness = float4(s.normal, s.roughness);
    o.specular = float4(specularWeight * occlusion, 1.0);
    return o;
#else
    return float4(color, saturate(alpha));
#endif
}
#endif
