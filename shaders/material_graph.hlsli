// Interpreter of material graphs (engine/assets/MaterialGraph.hpp): a program of instructions
// in a float texture (two texels each: (op, dst, a, b), (c, p0, p1, p2)) over 32 float4
// registers. Registers 0-6 hold the surface after the fragment program, 7 the world offset
// after the vertex program. Included after common.hlsli.
#ifndef LUMORA_MATERIAL_GRAPH_HLSLI
#define LUMORA_MATERIAL_GRAPH_HLSLI

#define GRAPH_REGISTERS 32
#define GRAPH_BASE_COLOR 0
#define GRAPH_OPACITY 1
#define GRAPH_METALLIC 2
#define GRAPH_ROUGHNESS 3
#define GRAPH_NORMAL 4
#define GRAPH_EMISSIVE 5
#define GRAPH_OCCLUSION 6
#define GRAPH_WORLD_OFFSET 7

static float4 gGraph[GRAPH_REGISTERS];

/// What the program can read about the surface. The vertex program sees only positions and
/// time (every pass computes the offset alike, so depth matches).
struct GraphInputs {
    float2 uv0;
    float2 uv1;
    float3 worldPosition;
    float3 localPosition;
    float3 normal;  // world, geometric
    float3 view;    // towards the camera
};

uint graphTextureIndex(GpuObject object, uint slot) {
    uint4 group = slot < 4u ? object.graphTextures0 : object.graphTextures1;
    uint i = slot & 3u;
    return i == 0u ? group.x : i == 1u ? group.y : i == 2u ? group.z : group.w;
}

float graphHash(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float graphNoise(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = graphHash(i), n100 = graphHash(i + float3(1, 0, 0)), n010 = graphHash(i + float3(0, 1, 0)), n110 = graphHash(i + float3(1, 1, 0));
    float n001 = graphHash(i + float3(0, 0, 1)), n101 = graphHash(i + float3(1, 0, 1)), n011 = graphHash(i + float3(0, 1, 1)), n111 = graphHash(i + float3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y), lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

float4 graphSample(GpuObject object, uint slot, float2 uv, bool normalMap, float strength) {
    uint index = graphTextureIndex(object, slot);
    if (index == NO_TEXTURE) {
        return normalMap ? float4(0.0, 0.0, 1.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0);
    }
#if defined(LUMORA_STAGE_FRAG)
    float4 t = gTextures[index].Sample(gSamplers[gFrame.counts.z], uv);
#else
    // Vertex programs read the nearest texel of the top level, wrapping: glslang breaks samplers
    // from gSamplers in vertex shaders (it aliases them with the comparison samplers).
    uint width, height, levels;
    gTextures[index].GetDimensions(0, width, height, levels);
    int2 texel = int2(frac(uv) * float2(width, height));
    float4 t = gTextures[index].Load(int3(texel, 0));
#endif
    if (normalMap) {
        // x and y from the map (BC5 keeps only those), z rebuilt; the strength tilts it.
        float2 xy = t.xy * 2.0 - 1.0;
        float z = sqrt(saturate(1.0 - dot(xy, xy)));
        return float4(normalize(float3(xy * strength, z)), 1.0);
    }
    return t;
}

void runMaterialGraph(GpuObject object, uint program, uint count, GraphInputs g) {
    float time = gFrame.cameraPositionTime.w;  // (no local Texture2D: glslang mis-compiles it in vertex shaders)
    [loop] for (uint k = 0; k < count; ++k) {
        float4 i0 = gTextures[program].Load(int3(int(k * 2u), 0, 0));
        float4 i1 = gTextures[program].Load(int3(int(k * 2u + 1u), 0, 0));
        uint op = (uint)i0.x;
        uint dst = min((uint)i0.y, GRAPH_REGISTERS - 1u);
        float4 a = gGraph[min((uint)i0.z, GRAPH_REGISTERS - 1u)];
        float4 b = gGraph[min((uint)i0.w, GRAPH_REGISTERS - 1u)];
        float4 c = gGraph[min((uint)i1.x, GRAPH_REGISTERS - 1u)];
        float4 r = 0.0;
        switch (op) {
            case 1u: r = i1; break;  // constant
            case 2u: {               // input
                uint which = (uint)i1.y;
                r = which == 0u ? float4(g.uv0, 0.0, 1.0)
                  : which == 1u ? float4(g.uv1, 0.0, 1.0)
                  : which == 2u ? float4(g.worldPosition, 1.0)
                  : which == 3u ? float4(g.normal, 0.0)
                  : which == 4u ? float4(g.view, 0.0)
                  : which == 5u ? time.xxxx
                                : float4(g.localPosition, 1.0);
                break;
            }
            case 3u: r = graphSample(object, (uint)i1.y, a.xy, i1.z > 0.5, b.x); break;
            case 4u: r = a + b; break;
            case 5u: r = a - b; break;
            case 6u: r = a * b; break;
            case 7u: r = a / (abs(b) > 1e-6 ? b : (b < 0.0 ? -1e-6 : 1e-6)); break;
            case 8u: r = min(a, b); break;
            case 9u: r = max(a, b); break;
            case 10u: r = pow(max(a, 0.0), b); break;
            case 11u: r = lerp(a, b, c); break;
            case 12u: r = clamp(a, b, c); break;
            case 13u: r = 1.0 - a; break;
            case 14u: r = abs(a); break;
            case 15u: r = frac(a); break;
            case 16u: r = sin(a); break;
            case 17u: r = dot(a.xyz, b.xyz).xxxx; break;
            case 18u: {  // splat a component
                uint i = (uint)i1.y;
                float v = i == 0u ? a.x : i == 1u ? a.y : i == 2u ? a.z : a.w;
                r = v.xxxx;
                break;
            }
            case 19u: r = float4(a.x, b.x, c.x, 1.0); break;
            case 20u: r = saturate(a); break;
            case 21u: r = pow(1.0 - saturate(dot(g.normal, g.view)), max(a.x, 0.0)).xxxx; break;
            case 22u: r = smoothstep(a.x, b.x, g.normal.y).xxxx; break;
            case 23u: r = graphNoise(a.xyz * b.x).xxxx; break;
            case 24u: {  // wind: sway along a horizontal direction, two frequencies, phased by position
                float phase = dot(g.worldPosition.xz, float2(0.35, 0.27));
                float t = time * b.x;
                float sway = sin(t + phase) * 0.7 + sin(t * 2.3 + phase * 1.7) * 0.3;
                r = float4(float3(0.928, 0.0, 0.371) * sway * a.x * c.x, 0.0);
                break;
            }
            case 25u: r = float4(a.xy * b.xy + c.xy, 0.0, 1.0); break;
            case 26u: r = float4(a.xy + b.xy * time, 0.0, 1.0); break;
            case 27u: r = a; break;
            case 28u: r = length(a.xyz).xxxx; break;
            case 29u: r = smoothstep(a, b, c); break;
            case 30u: r = float4(normalize(a.xyz + float3(0.0, 0.0, 1e-8)), 0.0); break;
            default: r = 0.0; break;
        }
        gGraph[dst] = r;
    }
}

/// The world-space offset the object's vertex program gives a vertex (zero without one).
float3 graphWorldOffset(GpuObject object, float3 localPosition, float3 worldPosition) {
    if (object.graph.z == NO_TEXTURE) {
        return 0.0;
    }
    GraphInputs g;
    g.uv0 = 0.0;
    g.uv1 = 0.0;
    g.worldPosition = worldPosition;
    g.localPosition = localPosition;
    g.normal = 0.0;
    g.view = 0.0;
    gGraph[GRAPH_WORLD_OFFSET] = 0.0;
    runMaterialGraph(object, object.graph.z, object.graph.w, g);
    return gGraph[GRAPH_WORLD_OFFSET].xyz;
}

#endif
