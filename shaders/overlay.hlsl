// Unlit overlay geometry (editor gizmos, debug shapes) drawn on top of the tonemapped image.
// Vertex colours are authored in sRGB. On sRGB render targets the hardware encodes on store, so
// we linearise first; gPush.flags bit0 marks a UNORM target where the value is stored as is.
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
};

struct VSOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput o;
    o.position = mul(gFrame.viewProjection, float4(input.position, 1.0));
    o.color = input.color;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    float3 c = input.color.rgb;
    if ((gPush.flags & 1u) == 0u) {
        c = pow(max(c, 0.0.xxx), 2.2);
    }
    return float4(c, input.color.a);
}
