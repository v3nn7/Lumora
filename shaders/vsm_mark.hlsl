// Virtual shadow map page requests from the visible surfaces (compute, after the depth
// prepass): every pixel marks the page of the finest level its shadow lookup uses (see
// vsmShadow in shadows.hlsli), and the neighbouring pages its filter and normal offset reach.
// The CPU reads the marks back a couple of frames later and keeps exactly those pages resident.
// gPush: index0 = depth (bindless).
// Output (set 1, u0): R32_UINT, kPagesPerLevel x (kPagesPerLevel * levels): non-zero = requested;
// the page slot is the absolute page modulo kPagesPerLevel, as in the page table.
#define LUMORA_COMPUTE 1
#include "common.hlsli"

[[vk::binding(0, 1)]] [[spv::format_r32ui]] RWTexture2D<uint> gRequests;

void mark(uint level, float2 pageCoords, float pages) {
    float2 page = floor(pageCoords);
    int2 slot = int2(page - floor(page / pages) * pages);  // positive modulo
    InterlockedOr(gRequests[int2(slot.x, slot.y + (int)(level * (uint)pages))], 1u);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const int2 size = int2(gFrame.viewportSize.xy);
    const int2 p = int2(id.xy);
    if (any(p >= size)) {
        return;
    }
    float depth = gTextures[gPush.index0].Load(int3(p, 0)).r;
    if (depth <= 0.0) {
        return;  // sky
    }
    float2 uv = (float2(p) + 0.5) * gFrame.viewportSize.zw;
    float4 w = mul(gFrame.inverseViewProjection, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 position = w.xyz / w.w;
    // The finest level whose reach covers this view depth (vsmFirstLevel).
    float viewDepth = -mul(gFrame.view, float4(position, 1.0)).z;
    float ratio = max(viewDepth, 1e-3) / (gFrame.vsmParams.y * gFrame.vsmParams.z);
    uint level = (uint)clamp(ceil(log2(max(ratio, 1.0))), 0.0, gFrame.vsmParams.x - 1.0);
    float pages = gFrame.vsmParams.w;
    float pageWorld = 2.0 * gFrame.vsmParams.y * exp2((float)level) / pages;
    float2 ls = mul(gFrame.vsmLightView, float4(position, 1.0)).xy;
    // Filter (1.5 texels) plus normal offset (up to 3 texels) around the point.
    float reach = pageWorld / gFrame.vsmDepth.z * 4.0;
    mark(level, (ls + float2(-reach, -reach)) / pageWorld, pages);
    mark(level, (ls + float2(reach, -reach)) / pageWorld, pages);
    mark(level, (ls + float2(-reach, reach)) / pageWorld, pages);
    mark(level, (ls + float2(reach, reach)) / pageWorld, pages);
}
