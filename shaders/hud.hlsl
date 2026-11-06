// HUD: pixel-space rectangles and text (glyphs from the font atlas), alpha blended on top of
// the final image. uv.x < 0 marks a solid rectangle. gPush: index0 = font atlas, index1 =
// sampler, flags bit0 = UNORM target (colours stay as authored; sRGB targets decode first),
// bit1 = index0 is a colour picture (sprites) that the vertex colour tints, not glyph coverage.
#include "common.hlsli"

struct VSInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput o;
    o.position = toTarget(float4(input.position * gFrame.viewportSize.zw * 2.0 - 1.0, 0.0, 1.0));
    o.uv = input.uv;
    o.color = input.color;
    return o;
}

#if defined(LUMORA_STAGE_FRAG)
float4 PSMain(VSOutput input) : SV_Target0 {
    float coverage = 1.0;
    float3 c = input.color.rgb;
    if ((gPush.flags & 2u) != 0u) {
        // Straight alpha; the atlas carries colour into its transparent texels so edges do not darken.
        float4 picture = gTextures[gPush.index0].SampleLevel(gSamplers[gPush.index1], input.uv, 0.0);
        c *= picture.rgb;
        coverage = picture.a;
    } else if (input.uv.x >= 0.0) {
        coverage = gTextures[gPush.index0].SampleLevel(gSamplers[gPush.index1], input.uv, 0.0).r;
    }
    if ((gPush.flags & 1u) == 0u) {
        c = pow(max(c, 0.0.xxx), 2.2);
    }
    return float4(c, input.color.a * coverage);
}
#endif
