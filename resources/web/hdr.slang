// Portable HDR compositor. Operator and atmospheric math are the native shader families.
#include "../../src/shaders/include/Tonemap/TonemapOperators.slangh"
#include "../../src/shaders/include/Sky.slangh"
#include "../../src/shaders/include/FullScreenTriangle.slangh"

struct HdrUniforms
{
    float4 view[4];
    float4 settings; // native operator, linear exposure multiplier, camera mode, stop count
    float4 extentAngleTime;
    float4 projection;
    float4 sunDirectionIntensity;
    float4 sunColor;
    float4 horizon;
    float4 zenith;
    float4 voidColor;
    float4 skyParams;
    float4 shadingParams;
    float4 camera;
    float4 stops[8];
};
[[vk::binding(0, 0)]] ConstantBuffer<HdrUniforms> frame;
[[vk::binding(1, 0)]] Texture2D<float4> scene;

[shader("fragment")]
float4 backgroundMain(float4 position : SV_Position) : SV_Target0
{
    if (uint(frame.settings.z) != 2u) return float4(0);
    // WebGPU framebuffer coordinates are y-down; match native Vulkan NDC reconstruction.
    const float2 ndc = float2(position.x / frame.extentAngleTime.x * 2 - 1,
                             position.y / frame.extentAngleTime.y * 2 - 1);
    const float3 viewDir = normalize(float3(ndc / frame.projection.xy, -1));
    const float3 worldDir = normalize(float3(dot(viewDir, frame.view[0].xyz),
                                              dot(viewDir, frame.view[1].xyz),
                                              dot(viewDir, frame.view[2].xyz)));
    const float3 sunDir = normalize(frame.sunDirectionIntensity.xyz);
    float3 sky = EvalSky(worldDir, sunDir, frame.sunColor.rgb, frame.sunDirectionIntensity.w,
                        frame.horizon.rgb, frame.zenith.rgb, frame.voidColor.rgb,
                        frame.skyParams, frame.shadingParams.y);
    sky = ApplySkyClouds(sky, worldDir, sunDir, frame.sunColor.rgb, frame.sunDirectionIntensity.w,
                         frame.horizon.rgb, frame.zenith.rgb, frame.camera.xyz,
                         frame.shadingParams.y, frame.shadingParams.z, frame.extentAngleTime.w);
    sky += (Hash12(floor(position.xy)) - .5f) * (1.0f / 1024.0f) * saturate(sunDir.y * .5f + .5f);
    return float4(max(sky, float3(0)), 1);
}

float3 Background(float2 uv)
{
    if (uint(frame.settings.z) == 0u || frame.settings.w <= 1) return frame.stops[0].rgb;
    const float2 dir = float2(sin(frame.extentAngleTime.z), cos(frame.extentAngleTime.z));
    const float t = saturate(dot(uv - .5f, dir) + .5f);
    float3 color = frame.stops[0].rgb;
    for (uint index = 1; index < uint(frame.settings.w); ++index)
    {
        const float p0 = frame.stops[index - 1].w;
        const float p1 = frame.stops[index].w;
        const float segment = saturate((t - p0) / max(p1 - p0, 1e-5f));
        color = t >= p0 ? lerp(frame.stops[index - 1].rgb, frame.stops[index].rgb, segment) : color;
    }
    return color;
}

// Native tonemap's final one-LSB TPDF dither, after the display-space background.
float Hash12Dither(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}
float3 DitherTriangular(float2 pixel)
{
    const float r = Hash12Dither(pixel) - Hash12Dither(pixel + float2(37.0f, 17.0f));
    const float g = Hash12Dither(pixel + float2(11.0f, 91.0f)) - Hash12Dither(pixel + float2(59.0f, 3.0f));
    const float b = Hash12Dither(pixel + float2(73.0f, 41.0f)) - Hash12Dither(pixel + float2(23.0f, 67.0f));
    return float3(r, g, b) * (1.0f / 255.0f);
}

[shader("fragment")]
float4 compositeMain(float4 position : SV_Position) : SV_Target0
{
    const int2 coord = int2(position.xy);
    const float4 hdr = scene.Load(int3(coord, 0));
    float3 output = pow(max(TonemapByIndex(uint(frame.settings.x), hdr.rgb * frame.settings.y), float3(0)), float3(1.0f / 2.2f));
    const bool cameraBackground = uint(frame.settings.z) != 2u;
    if (cameraBackground)
    {
        output = lerp(Background((float2(coord) + .5f) / frame.extentAngleTime.xy), output, saturate(hdr.a));
    }
    return float4(output + DitherTriangular(float2(coord)), cameraBackground ? 1.0f : hdr.a);
}
