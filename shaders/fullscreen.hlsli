#ifndef LUMORA_FULLSCREEN_HLSLI
#define LUMORA_FULLSCREEN_HLSLI

struct FullscreenOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

/// Single oversized triangle covering the viewport; uv (0,0) is the top-left corner.
FullscreenOut fullscreenTriangle(uint id) {
    FullscreenOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

#endif
