// Depth-only pass rendering shadow casters from the sun's point of view into one cascade tile
// of the shadow atlas (gPush.index1 = cascade), or from a spot/point light into a tile of the local
// shadow atlas (flags bit0, index1 = tile).
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

float4 VSMain(VSInput input, uint instance : SV_InstanceID) : SV_Position {
    GpuObject object = gObjects[gPush.index0 + instance];
    float3 position = input.position;
#if defined(LUMORA_SKINNED)
    position = mul(skinMatrix(input.joints, input.weights, (uint)object.flags.z), float4(position, 1.0)).xyz;
#endif
    // flags bit0: a spot/point-light tile of the local shadow atlas instead of a sun cascade.
    float4x4 lightViewProjection = (gPush.flags & 1u) != 0u ? gFrame.localShadowMatrices[gPush.index1]
                                                           : gFrame.cascadeViewProjection[gPush.index1];
    return toTarget(mul(lightViewProjection, mul(object.world, float4(position, 1.0))));
}
