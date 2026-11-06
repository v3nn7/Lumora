// Depth prepass: the camera's depth before shading, read by ambient occlusion; the opaque pass
// then shades each pixel once (depth test GREATER_OR_EQUAL, no writes).
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
#if defined(LUMORA_SKINNED)
    [[vk::location(4)]] uint4 joints : BLENDINDICES;
    [[vk::location(5)]] float4 weights : BLENDWEIGHT;
#endif
};

float4 VSMain(VSInput input) : SV_Position {
    GpuObject object = gObjects[gPush.index0];
    float3 position = input.position;
#if defined(LUMORA_SKINNED)
    position = mul(skinMatrix(input.joints, input.weights, (uint)object.flags.z), float4(position, 1.0)).xyz;
#endif
    // Same expression as mesh.hlsl so both passes produce the same depth.
    float4 world = mul(object.world, float4(position, 1.0));
    return mul(gFrame.viewProjection, world);
}
