// Depth-only pass rendering shadow casters from the sun's point of view into one cascade tile
// of the shadow atlas (gPush.index1 = cascade).
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

float4 VSMain(VSInput input) : SV_Position {
    GpuObject object = gObjects[gPush.index0];
    return mul(gFrame.cascadeViewProjection[gPush.index1], mul(object.world, float4(input.position, 1.0)));
}
