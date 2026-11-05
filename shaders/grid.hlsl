#include "common.hlsli"
#include "fullscreen.hlsli"

struct GridOutput {
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

FullscreenOut VSMain(uint id : SV_VertexID) {
    return fullscreenTriangle(id);
}

float gridLines(float2 coord) {
    float2 derivative = max(fwidth(coord), 1e-5.xx);
    float2 g = abs(frac(coord - 0.5) - 0.5) / derivative;
    return 1.0 - min(min(g.x, g.y), 1.0);
}

GridOutput PSMain(FullscreenOut input) {
    float3 origin = gFrame.cameraPositionTime.xyz;
    float3 dir = viewRay(input.uv);
    if (abs(dir.y) < 1e-6) {
        discard;
    }
    float t = -origin.y / dir.y;
    if (t <= 0.0) {
        discard;
    }
    float3 p = origin + dir * t;
    float cell = max(gFrame.gridParams.y, 1e-3);
    float minor = gridLines(p.xz / cell);
    float major = gridLines(p.xz / (cell * 10.0));

    float3 color = lerp(0.35.xxx, 0.55.xxx, major);
    float alpha = max(minor * 0.25, major * 0.5);

    // Highlight world axes: X axis (z = 0) red, Z axis (x = 0) blue.
    float2 axisDerivative = max(fwidth(p.xz), 1e-5.xx);
    float xAxis = 1.0 - min(abs(p.z) / axisDerivative.y, 1.0);
    float zAxis = 1.0 - min(abs(p.x) / axisDerivative.x, 1.0);
    color = lerp(color, float3(0.85, 0.2, 0.2), xAxis);
    color = lerp(color, float3(0.2, 0.4, 0.9), zAxis);
    alpha = max(alpha, max(xAxis, zAxis) * 0.8);

    // Fade with distance and at grazing angles to hide aliasing.
    float fade = 1.0 - saturate(t / max(gFrame.gridParams.z, 1.0));
    fade *= saturate(abs(dir.y) * 6.0);
    alpha *= fade * fade;
    if (alpha < 0.002) {
        discard;
    }

    // Pull the depth slightly towards the camera so the grid wins against coplanar ground.
    float3 biased = p - dir * (0.002 * t + 0.001);
    float4 clip = mul(gFrame.viewProjection, float4(biased, 1.0));
    GridOutput o;
    o.color = float4(color * 0.6, alpha);
    o.depth = clip.z / clip.w;
    return o;
}
