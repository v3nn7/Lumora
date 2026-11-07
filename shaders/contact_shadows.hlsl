// Screen-space contact shadows (compute, after the depth prepass): from each pixel a short ray
// towards the sun is marched through the depth buffer; whatever it passes behind shadows the
// pixel. Catches the small contact shadows shadow maps are too coarse for (feet on the ground,
// objects on tables, folds), and fades out towards the screen edges and the ray's end.
// gPush: index0 = depth (bindless), param0 = ray length (m at 10 m from the camera; it scales
//        with the distance), param1 = frame number.
// Output (set 1, u0): R8 visibility (1 = lit).
#define LUMORA_COMPUTE 1
#include "common.hlsli"

[[vk::binding(0, 1)]] [[spv::format_r8]] RWTexture2D<float> gOut DX_REGISTER_SPACE(u0, space1);

static const int kSteps = 16;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const int2 size = int2(gFrame.viewportSize.xy);
    const int2 p = int2(id.xy);
    if (any(p >= size)) {
        return;
    }
    Texture2D depthTex = gTextures[gPush.index0];
    const float depth = depthTex.Load(int3(p, 0)).r;
    if (depth <= 0.0 || gFrame.sunDirection.w <= 0.0) {
        gOut[p] = 1.0;
        return;
    }
    const float2 uv = (float2(p) + 0.5) * gFrame.viewportSize.zw;
    float4 w = mul(gFrame.inverseViewProjection, float4(uv * 2.0 - 1.0, depth, 1.0));
    const float3 position = w.xyz / w.w;
    const float3 camera = gFrame.cameraPositionTime.xyz;
    const float distance = length(position - camera);
    const float3 toSun = normalize(gFrame.sunDirection.xyz);
    const float rayLength = gPush.param0 * clamp(distance / 10.0, 0.3, 4.0);
    const float jitter = frac(52.9829189 * frac(dot(float2(p) + gPush.param1 * 5.588238, float2(0.06711056, 0.00583715))));
    const float near = mul(gFrame.viewProjection, float4(position, 1.0)).z;  // reverse-Z infinite: clip z = near
    float visibility = 1.0;
    [loop] for (int i = 1; i <= kSteps; ++i) {
        float t = (float(i) - jitter) / float(kSteps);
        float3 q = position + toSun * (rayLength * t + distance * 0.002);
        float4 clip = mul(gFrame.viewProjection, float4(q, 1.0));
        if (clip.w <= 0.0) {
            break;
        }
        float2 quv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(quv <= 0.0) || any(quv >= 1.0)) {
            break;
        }
        float sceneDepth = depthTex.Load(int3(int2(quv * gFrame.viewportSize.xy), 0)).r;
        float rayDistance = clip.w;
        float sceneDistance = sceneDepth > 0.0 ? near / sceneDepth : 1e9;
        float behind = rayDistance - sceneDistance;
        // Behind a surface, but not so far that the ray passed under a separate object.
        if (behind > 0.02 * distance / 10.0 && behind < 0.05 + 0.03 * sceneDistance) {
            float2 edge = smoothstep(0.0, 0.05, min(quv, 1.0 - quv));
            visibility = 1.0 - edge.x * edge.y * (1.0 - t * t);
            break;
        }
    }
    gOut[p] = visibility;
}
