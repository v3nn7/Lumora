// Decals: a box (the entity's unit cube) projects a texture down its local -Y axis onto whatever
// the depth buffer holds inside it, multiplying the lit colour (dirt, cracks, paint, stains).
// gPush: index0 = object (world, inverse world, colour + opacity, texture), index1 = depth,
//        index2 = sampler for the depth, flags unused.
#include "common.hlsli"

struct VSOutput {
    float4 position : SV_Position;
};

VSOutput VSMain([[vk::location(0)]] float3 position : POSITION) {
    VSOutput o;
    GpuObject object = gObjects[gPush.index0];
    o.position = toTarget(mul(gFrame.viewProjection, mul(object.world, float4(position, 1.0))));
    return o;
}

#if defined(LUMORA_STAGE_FRAG)
float4 PSMain(VSOutput input) : SV_Target0 {
    GpuObject object = gObjects[gPush.index0];
    float2 uv = input.position.xy * gFrame.viewportSize.zw;
    float depth = gTextures[gPush.index1].SampleLevel(gSamplers[gPush.index2], uv, 0.0).r;
    if (depth <= 0.0) {
        discard;  // sky
    }
    float4 world = mul(gFrame.inverseViewProjection, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 local = mul(object.normalMatrix, float4(world.xyz / world.w, 1.0)).xyz;  // normalMatrix = inverse world
    if (any(abs(local) > 0.5)) {
        discard;
    }
    float4 color = float4(object.baseColorMetallic.rgb, 1.0);
    if (object.textures.x != NO_TEXTURE) {
        color *= gTextures[object.textures.x].SampleLevel(gSamplers[gFrame.counts.z], float2(local.x, -local.z) + 0.5, 0.0);
    }
    // Fade near the top and bottom of the box so surfaces leaving it do not end in a hard line.
    float fade = saturate((0.5 - abs(local.y)) * 8.0);
    float alpha = saturate(object.baseColorMetallic.a * color.a * fade);
    return float4(lerp(1.0.xxx, color.rgb, alpha), 1.0);  // multiplied onto the scene
}
#endif
