// Depth-only pass rendering shadow casters from the sun's point of view into one cascade tile
// of the shadow atlas (gPush.index1 = cascade), or from a spot/point light into a tile of the local
// shadow atlas (flags bit0, index1 = tile), or into a page of the virtual shadow map (flags bit1).
// With LUMORA_MASKED the base colour's alpha cuts holes below the object's cutoff.
#include "common.hlsli"
#include "material_graph.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
#if defined(LUMORA_MASKED)
    [[vk::location(4)]] float2 uv1 : UVSET;  // masked variants are never skinned
#endif
#if defined(LUMORA_SKINNED)
    [[vk::location(4)]] uint4 joints : BLENDINDICES;
    [[vk::location(5)]] float4 weights : BLENDWEIGHT;
#endif
};

float4 lightClip(GpuObject object, VSInput input) {
    float3 position = input.position;
#if defined(LUMORA_SKINNED)
    position = mul(skinMatrix(input.joints, input.weights, (uint)object.flags.z), float4(position, 1.0)).xyz;
#endif
    // The material graph's vertex offset (wind), as in the other passes.
    float4 world = mul(object.world, float4(position, 1.0));
    world.xyz += graphWorldOffset(object, position, world.xyz);
    // flags bit1: a page of the virtual shadow map (param0/1 page centre, param2 1/half size).
    if ((gPush.flags & 2u) != 0u) {
        float3 ls = mul(gFrame.vsmLightView, world).xyz;
        float2 xy = (ls.xy - float2(gPush.param0, gPush.param1)) * gPush.param2;
        return toTarget(float4(xy, (ls.z - gFrame.vsmDepth.x) * gFrame.vsmDepth.y, 1.0));
    }
    // flags bit0: a spot/point-light tile of the local shadow atlas instead of a sun cascade.
    float4x4 lightViewProjection = (gPush.flags & 1u) != 0u ? gFrame.localShadowMatrices[gPush.index1]
                                                           : gFrame.cascadeViewProjection[gPush.index1];
    return toTarget(mul(lightViewProjection, world));
}

#if defined(LUMORA_MASKED)
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint objectIndex : TEXCOORD1;
    float2 uv1 : TEXCOORD2;            // for material graphs
    float3 worldPosition : TEXCOORD3;
    float3 normal : TEXCOORD4;
};

VSOutput VSMain(VSInput input, uint instance : SV_InstanceID) {
    VSOutput o;
    o.objectIndex = gPush.index0 + instance;
    o.position = lightClip(gObjects[o.objectIndex], input);
    const GpuObject object = gObjects[o.objectIndex];
    o.uv = (asuint(object.attenuation.w) & 1u) != 0u ? input.uv1 : materialUv(object, input.uv);
    o.uv1 = input.uv1;
    float3 world = mul(object.world, float4(input.position, 1.0)).xyz;
    o.worldPosition = world + graphWorldOffset(object, input.position, world);
    o.normal = mul((float3x3)object.normalMatrix, input.normal);
    return o;
}

#if defined(LUMORA_STAGE_FRAG)
void PSMain(VSOutput input) {
    GpuObject object = gObjects[input.objectIndex];
    if (object.graph.x != NO_TEXTURE) {
        // A material graph's opacity, as the opaque pass computes it.
        GraphInputs g;
        g.uv0 = input.uv;
        g.uv1 = input.uv1;
        g.worldPosition = input.worldPosition;
        g.localPosition = 0.0;
        g.normal = normalize(input.normal);
        g.view = normalize(gFrame.cameraPositionTime.xyz - input.worldPosition);
        runMaterialGraph(object, object.graph.x, object.graph.y, g);
        if (gGraph[GRAPH_OPACITY].x < object.surface.x) {
            discard;
        }
        return;
    }
    if (object.textures.x != NO_TEXTURE &&
        gTextures[object.textures.x].Sample(gSamplers[gFrame.counts.z], input.uv).a < object.surface.x) {
        discard;
    }
}
#endif
#else
float4 VSMain(VSInput input, uint instance : SV_InstanceID) : SV_Position {
    return lightClip(gObjects[gPush.index0 + instance], input);
}
#endif
