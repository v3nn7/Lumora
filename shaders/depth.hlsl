// Depth prepass: the camera's depth before shading, read by ambient occlusion; the opaque pass
// then shades each pixel once (depth test GREATER_OR_EQUAL, no writes).
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_Position {
    GpuObject object = gObjects[gPush.index0];
    // Same expression as mesh.hlsl so both passes produce the same depth.
    float4 world = mul(object.world, float4(input.position, 1.0));
    return mul(gFrame.viewProjection, world);
}
