#include "common.hlsli"
#include "fullscreen.hlsli"

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

float4 PSMain(FullscreenOut input) : SV_Target0 {
    float3 dir = viewRay(input.uv);
    float3 color = skyGradient(dir);
    if (gFrame.sunDirection.w > 0.0) {
        float d = dot(dir, gFrame.sunDirection.xyz);
        float disk = smoothstep(0.99985, 0.99993, d);
        float glow = pow(saturate(d), 300.0) * 0.5 + pow(saturate(d), 12.0) * 0.04;
        color += gFrame.sunColor.rgb * (disk * 25.0 + glow);
    }
    return float4(color, 1.0);
}
