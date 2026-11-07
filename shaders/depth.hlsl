// Depth prepass: the camera's depth before shading, read by ambient occlusion; the opaque pass
// then shades each pixel once (depth test GREATER_OR_EQUAL, no writes). It also writes each
// pixel's motion (RG16F, NDC units, jitter removed): where the surface was on screen last frame,
// from the previous camera and the object's previous transform; temporal anti-aliasing reads it.
// With LUMORA_MASKED the base colour's alpha cuts holes below the object's cutoff (leaves,
// fences), as the opaque pass does.
#include "common.hlsli"
#include "material_graph.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
#if defined(LUMORA_MASKED)
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(4)]] float2 uv1 : UVSET;  // masked variants are never skinned
#endif
#if defined(LUMORA_SKINNED)
    [[vk::location(4)]] uint4 joints : BLENDINDICES;
    [[vk::location(5)]] float4 weights : BLENDWEIGHT;
#endif
};

struct VSOutput {
    float4 position : SV_Position;
    float4 current : TEXCOORD0;   // clip position this frame (jittered)
    float4 previous : TEXCOORD1;  // clip position last frame (not jittered)
#if defined(LUMORA_MASKED)
    float2 uv : TEXCOORD2;
    nointerpolation uint objectIndex : TEXCOORD3;
    float2 uv1 : TEXCOORD4;            // for material graphs
    float3 worldPosition : TEXCOORD5;
    float3 normal : TEXCOORD6;
#endif
};

VSOutput VSMain(VSInput input, uint instance : SV_InstanceID) {
    const uint objectIndex = gPush.index0 + instance;
    GpuObject object = gObjects[objectIndex];
    float3 position = input.position;
#if defined(LUMORA_SKINNED)
    // (Last frame's pose is not kept: skinned motion is the object's and the camera's.)
    position = mul(skinMatrix(input.joints, input.weights, (uint)object.flags.z), float4(position, 1.0)).xyz;
#endif
    // Same expression as mesh.hlsl so both passes produce the same depth.
    float4 world = mul(object.world, float4(position, 1.0));
    // The material graph's vertex offset (wind). Last frame's position moves with it (the
    // offset's own motion is not tracked).
    const float3 offset = graphWorldOffset(object, position, world.xyz);
    world.xyz += offset;
    VSOutput o;
    o.current = mul(gFrame.viewProjection, world);
    o.position = toTarget(o.current);
    o.previous = mul(gFrame.previousViewProjection, mul(object.previousWorld, float4(position, 1.0)) + float4(offset, 0.0));
#if defined(LUMORA_MASKED)
    o.uv = (asuint(object.attenuation.w) & 1u) != 0u ? input.uv1 : materialUv(object, input.uv);
    o.objectIndex = objectIndex;
    o.uv1 = input.uv1;
    o.worldPosition = world.xyz;
    o.normal = mul((float3x3)object.normalMatrix, input.normal);
#endif
    return o;
}

#if defined(LUMORA_STAGE_FRAG)
float2 PSMain(VSOutput input) : SV_Target0 {
#if defined(LUMORA_MASKED)
    GpuObject object = gObjects[input.objectIndex];
    if (object.graph.x != NO_TEXTURE) {
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
    } else if (object.textures.x != NO_TEXTURE &&
        gTextures[object.textures.x].Sample(gSamplers[gFrame.counts.z], input.uv).a < object.surface.x) {
        discard;
    }
#endif
    float2 current = input.current.xy / input.current.w - gFrame.jitter.xy;
    float2 previous = input.previous.xy / max(input.previous.w, 1e-6);
    return current - previous;
}
#endif
