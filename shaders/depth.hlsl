// Depth prepass: the camera's depth before shading, read by ambient occlusion; the opaque pass
// then shades each pixel once (depth test GREATER_OR_EQUAL, no writes). With LUMORA_MASKED the
// base colour's alpha cuts holes below the object's cutoff (leaves, fences), as the opaque pass does.
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
#if defined(LUMORA_MASKED)
    [[vk::location(2)]] float2 uv : TEXCOORD0;
#endif
#if defined(LUMORA_SKINNED)
    [[vk::location(4)]] uint4 joints : BLENDINDICES;
    [[vk::location(5)]] float4 weights : BLENDWEIGHT;
#endif
};

float4 clipPosition(GpuObject object, VSInput input) {
    float3 position = input.position;
#if defined(LUMORA_SKINNED)
    position = mul(skinMatrix(input.joints, input.weights, (uint)object.flags.z), float4(position, 1.0)).xyz;
#endif
    // Same expression as mesh.hlsl so both passes produce the same depth.
    float4 world = mul(object.world, float4(position, 1.0));
    return toTarget(mul(gFrame.viewProjection, world));
}

#if defined(LUMORA_MASKED)
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint objectIndex : TEXCOORD1;
};

VSOutput VSMain(VSInput input, uint instance : SV_InstanceID) {
    VSOutput o;
    o.objectIndex = gPush.index0 + instance;
    o.position = clipPosition(gObjects[o.objectIndex], input);
    o.uv = input.uv;
    return o;
}

#if defined(LUMORA_STAGE_FRAG)
void PSMain(VSOutput input) {
    GpuObject object = gObjects[input.objectIndex];
    if (object.textures.x != NO_TEXTURE &&
        gTextures[object.textures.x].Sample(gSamplers[gFrame.counts.z], input.uv).a < object.surface.x) {
        discard;
    }
}
#endif
#else
float4 VSMain(VSInput input, uint instance : SV_InstanceID) : SV_Position {
    return clipPosition(gObjects[gPush.index0 + instance], input);
}
#endif
