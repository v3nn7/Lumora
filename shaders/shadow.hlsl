// Depth-only pass rendering shadow casters from the sun's point of view into one cascade tile
// of the shadow atlas (gPush.index1 = cascade).
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
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
    return mul(gFrame.cascadeViewProjection[gPush.index1], mul(object.world, float4(position, 1.0)));
}
