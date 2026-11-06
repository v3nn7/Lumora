// Particles: camera-facing quads built on the CPU (corner in [-1, 1]), drawn into the HDR
// image after the opaque pass, depth-tested against it without writing depth. A soft round
// falloff gives each quad the shape of a puff. gPush.flags bit0 = additive (premultiplied).
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 corner : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 corner : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput o;
    o.position = toTarget(mul(gFrame.viewProjection, float4(input.position, 1.0)));
    o.corner = input.corner;
    o.color = input.color;
    return o;
}

#if defined(LUMORA_STAGE_FRAG)
float4 PSMain(VSOutput input) : SV_Target0 {
    const float r2 = dot(input.corner, input.corner);
    float shape = saturate(1.0 - r2);
    shape *= shape;  // soft edge, dense centre
    const float alpha = input.color.a * shape;
    if ((gPush.flags & 1u) != 0u) {
        return float4(input.color.rgb * alpha, 0.0);  // additive: light adds up
    }
    return float4(input.color.rgb, alpha);
}
#endif
