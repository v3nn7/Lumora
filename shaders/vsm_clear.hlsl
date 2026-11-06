// Clears one page of the virtual shadow map's pool to the far depth: a triangle over the
// page's viewport at depth 1 (the pass draws it with depth test "always").
#include "common.hlsli"

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 1.0, 1.0);
}
