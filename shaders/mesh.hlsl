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
};

VSOutput VSMain(VSInput input) {
    GpuObject object = gObjects[gPush.index0];
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
    o.position = mul(gFrame.viewProjection, world);
    o.worldPosition = world.xyz;
    o.normal = mul((float3x3)object.normalMatrix, normal);
    o.uv = input.uv;
    o.tangent = float4(mul((float3x3)object.world, tangent), input.tangent.w);
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

// ---- Sun shadows: 4 cascades in a 2x2 depth atlas ---------------------------------------

static const float2 kCascadeTile[SHADOW_CASCADES] = {float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
                                                      float2(1.0, 1.0)};

/// Atlas uv + depth of a world position in cascade c (tile-local uv is xy * 0.5 + 0.5).
float3 cascadeCoords(uint c, float3 worldPosition) {
    float4 clip = mul(gFrame.cascadeViewProjection[c], float4(worldPosition, 1.0));  // orthographic: w = 1
    return float3((kCascadeTile[c] + clip.xy * 0.5 + 0.5) * 0.5, clip.z);
}

/// d(atlas uv, depth) for a world-space step. The cascade transform is affine, so screen-space
/// derivatives of the world position (taken in uniform control flow) map through it exactly.
float3 cascadeDelta(uint c, float3 worldDelta) {
    float3 d = mul((float3x3)gFrame.cascadeViewProjection[c], worldDelta);
    return float3(d.xy * 0.25, d.z);
}

/// Optimised 7x7 PCF (Castano 2013, as used in The Witness): 16 bilinear depth comparisons
/// whose weights add up to a smooth 7x7 tent, so penumbrae are soft and free of noise.
/// Receiver-plane depth bias keeps sloped receivers from shadowing themselves across the kernel.
float filteredShadow(float3 coords, float2 depthSlope) {
    float size = gFrame.shadowParams.w;
    float inv = gFrame.shadowParams.z;
    uint smp = (uint)gFrame.shadowParams.y;
    float2 uv = coords.xy * size;
    float2 base = floor(uv + 0.5);
    float s = uv.x + 0.5 - base.x;
    float t = uv.y + 0.5 - base.y;
    base = (base - 0.5) * inv;
    float4 uw = float4(5.0 * s - 6.0, 11.0 * s - 28.0, -(11.0 * s + 17.0), -(5.0 * s + 1.0));
    float4 u = float4((4.0 * s - 5.0) / uw.x - 3.0, (4.0 * s - 16.0) / uw.y - 1.0, -(7.0 * s + 5.0) / uw.z + 1.0,
                      -s / uw.w + 3.0);
    float4 vw = float4(5.0 * t - 6.0, 11.0 * t - 28.0, -(11.0 * t + 17.0), -(5.0 * t + 1.0));
    float4 v = float4((4.0 * t - 5.0) / vw.x - 3.0, (4.0 * t - 16.0) / vw.y - 1.0, -(7.0 * t + 5.0) / vw.z + 1.0,
                      -t / vw.w + 3.0);
    float sum = 0.0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            float2 offset = float2(u[i], v[j]) * inv;
            float bias = clamp(dot(base + offset - coords.xy, depthSlope), -0.002, 0.002);
            sum += uw[i] * vw[j] *
                   gTextures[gPush.index1].SampleCmpLevelZero(gShadowSamplers[smp], base + offset, coords.z + bias);
        }
    }
    return sum / 2704.0;
}

/// Visibility in one cascade: normal offset scaled to the cascade's texel size (more at grazing
/// angles), then the filtered comparison with a receiver-plane bias from the depth gradient.
float cascadeVisibility(uint c, float3 worldPosition, float3 normal, float ndotl, float3 dpdx, float3 dpdy) {
    float offset = gFrame.cascadeTexelWorld[c] * (0.75 + 2.0 * (1.0 - ndotl));
    float3 coords = cascadeCoords(c, worldPosition + normal * offset);
    if (coords.z >= 1.0) {
        return 1.0;
    }
    float3 dx = cascadeDelta(c, dpdx);
    float3 dy = cascadeDelta(c, dpdy);
    float det = dx.x * dy.y - dx.y * dy.x;
    float2 slope = abs(det) > 1e-12 ? float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det : 0.0.xx;
    return filteredShadow(float3(coords.xy, coords.z - 0.0001), slope);
}

/// Sun visibility. The finest cascade whose tile holds the whole filter footprint is used; near
/// its border the next cascade is blended in so the resolution change has no visible seam, and
/// past the shadow distance shadows fade out.
/// Index of the finest cascade covering a world position (SHADOW_CASCADES if none), and how
/// close it is to that cascade's border (0 centre .. 1 border).
uint selectCascade(float3 worldPosition, out float edge) {
    // Keep 8 atlas texels (filter radius + normal offset) inside the tile: in tile NDC units.
    float limit = 1.0 - 32.0 * gFrame.shadowParams.z;
    edge = 0.0;
    [loop] for (uint c = 0; c < SHADOW_CASCADES; ++c) {
        float4 clip = mul(gFrame.cascadeViewProjection[c], float4(worldPosition, 1.0));
        // Depth 0.5 is the cascade centre (see fitSunShadow), so depth also blends near its ends.
        float extent = max(max(abs(clip.x), abs(clip.y)) / limit, abs(clip.z - 0.5) * 2.0);
        if (extent < 1.0) {
            edge = extent;
            return c;
        }
    }
    return SHADOW_CASCADES;
}

float sunShadow(float3 worldPosition, float3 normal, float3 toLight, float3 dpdx, float3 dpdy) {
    if ((gPush.flags & 1u) == 0u || gFrame.shadowParams.x < 0.5) {
        return 1.0;
    }
    float distance = length(worldPosition - gFrame.cameraPositionTime.xyz);
    float fade = saturate((distance - gFrame.shadowFade.x) / max(gFrame.shadowFade.y - gFrame.shadowFade.x, 1e-3));
    if (fade >= 1.0) {
        return 1.0;
    }
    float edge;
    uint first = selectCascade(worldPosition, edge);
    if (first == SHADOW_CASCADES) {
        return 1.0;
    }
    float ndotl = saturate(dot(normal, toLight));
    float visibility = cascadeVisibility(first, worldPosition, normal, ndotl, dpdx, dpdy);
    const float kBlendStart = 0.8;
    if (edge > kBlendStart) {
        float next = 1.0;  // past the last cascade: fade to lit
        if (first + 1 < SHADOW_CASCADES) {
            next = cascadeVisibility(first + 1, worldPosition, normal, ndotl, dpdx, dpdy);
        }
        visibility = lerp(visibility, next, smoothstep(kBlendStart, 1.0, edge));
    }
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
            s.normal = normalize(t * m.x + b * m.y + n * m.z);
        }
    }
    s.geometricNormal = geometricNormal;
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
    // Derivatives in uniform control flow: the normal-map branch above depends only on the draw.
    s.roughness = antialiasedRoughness(clamp(s.roughness, 0.02, 1.0), ddx(s.normal), ddy(s.normal));

    float3 color = evaluateAmbient(s);
    if ((gPush.flags & 4u) != 0u) {
        // Screen-space ambient occlusion (index2) darkens light from the sky in creases.
        color *= gTextures[gPush.index2].Load(int3(int2(input.position.xy), 0)).r;
    }
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
            if (attenuation > 0.0) {
                float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
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
        uint cascade = selectCascade(s.position, edge);
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
    return float4(color, 1.0);
}
#endif
