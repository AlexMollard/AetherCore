#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"
#include "include/ParticleBillboard.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct BillboardPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<GpuBillboardParticle> instances;
    uint instanceOffset;
    uint padding;
};

[[vk::push_constant]] BillboardPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint textureIndex : TEXCOORD2;
    nointerpolation uint displaySpace : TEXCOORD3;
    float3 viewPos : TEXCOORD4;            // view-space position of this point on the quad
    float2 ball : TEXCOORD5;               // unrotated quad position, -1..1 (lit shading)
    nointerpolation float size : TEXCOORD6;
    nointerpolation uint lit : TEXCOORD7;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const GpuBillboardParticle p = pc.instances.Ptr()[pc.instanceOffset + instanceId];
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    const float2 corner = corners[vertexId % 6u];

    // Centre in view space, expand on the view plane there, then project. Working in view
    // space makes the billboarding exact without depending on the view matrix's storage
    // layout: +x is screen right and +y is screen up by definition.
    const float3 centreView = mul(frame->view, float4(p.positionSize.xyz, 1.0)).xyz;
    float2 local = (corner - 0.5) * p.positionSize.w;
    const float radians = p.rotationDegrees * 3.14159265 / 180.0;
    const float s = sin(radians);
    const float c = cos(radians);
    local = float2(local.x * c - local.y * s, local.x * s + local.y * c);
    const float4 clip = mul(frame->proj, float4(centreView + float3(local, 0.0), 1.0));

    VSOutput output;
    output.position = clip;
    // uvRect is in image space (origin top-left, v down); quad corners are view space
    // (+y up). Flip v so the top of the image lands on the top of the quad.
    output.uv = lerp(p.uvRect.xy, p.uvRect.zw, float2(corner.x, 1.0 - corner.y));
    output.color = p.color;
    output.textureIndex = p.textureIndex;
    output.displaySpace = (p.blendMode & kBillboardDisplaySpace) != 0u ? 1u : 0u;
    output.viewPos = centreView + float3(local, 0.0);
    output.ball = (corner - 0.5) * 2.0;
    output.size = p.positionSize.w;
    output.lit = (p.blendMode & kBillboardLit) != 0u ? 1u : 0u;
    return output;
}

// Soft particles: fade a quad out over the last stretch in front of the opaque scene, so a
// puff sitting on the ground (or a wall) blends into it instead of showing a hard cut line.
// The fade distance follows the particle size (small sparks stay crisp), capped small so HUD
// sparkles, twinkles and trails that never touch geometry are unchanged.
static const float kSoftFraction = 0.25;
static const float kSoftMax = 0.6;

float SoftFade(float4 svPosition, float3 viewPos, float size)
{
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    const ResourceEntry depthEntry = LoadResourceEntry(frame->resourceTableAddr, kFrameResourceId_SceneDepth);
    if (depthEntry.type != kResourceTypeBindlessTexture || depthEntry.address == 0xFFFFFFFFu || depthEntry.width == 0u || depthEntry.height == 0u)
    {
        return 1.0;
    }
    const int2 texel = clamp(int2(svPosition.xy), int2(0, 0), int2(depthEntry.width - 1u, depthEntry.height - 1u));
    const float sceneDepth = g_textures[uint(depthEntry.address)].Load(int3(texel, 0)).r;
    const float2 ndc = (float2(texel) + 0.5) / float2(depthEntry.width, depthEntry.height) * 2.0 - 1.0;
    float4 sceneWorld = mul(frame->invViewProj, float4(ndc, sceneDepth, 1.0));
    sceneWorld.xyz /= max(sceneWorld.w, 1e-6);
    const float range = clamp(size * kSoftFraction, 0.02, kSoftMax);
    return saturate((distance(frame->cameraWorldPos.xyz, sceneWorld.xyz) - length(viewPos)) / range);
}

// Sun-lit ball: the quad is shaded as a sphere lit by the scene's sun with wrap lighting, a
// sky-tinted shadow side and a warm rim where the sun is behind it. The lights only tint (a
// quarter of their hue): the sun colour here is a saturated sunset orange that would dye every
// puff, and the smoke must keep its keyed colour (the nitro's stays green).
float3 LitBall(float2 ball)
{
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    const float r2 = saturate(dot(ball, ball));
    const float3 nView = normalize(float3(ball, sqrt(1.0 - r2) + 0.1));
    const float3 sunTrue = normalize(mul(frame->view, float4(normalize(frame->sunDirectionIntensity.xyz), 0.0)).xyz);
    // Cartoon key: a sun behind the puff still lights its crown from the sun's side of the screen
    // (a true back light would leave a flat dark disc); the back light shows as the rim below.
    const float3 sunView = normalize(float3(sunTrue.xy, max(sunTrue.z, 0.25)));
    const float3 sunHue = lerp(float3(1.0, 1.0, 1.0), frame->sunColor.rgb / max(max(frame->sunColor.r, max(frame->sunColor.g, frame->sunColor.b)), 1e-3), 0.25);
    const float3 skyHue = lerp(float3(1.0, 1.0, 1.0), frame->skyZenithColor.rgb / max(max(frame->skyZenithColor.r, max(frame->skyZenithColor.g, frame->skyZenithColor.b)), 1e-3), 0.3);
    const float wrap = saturate((dot(nView, sunView) + 0.45) / 1.45);
    const float back = saturate(-sunTrue.z) * r2; // sun behind the puff: its thin edges glow
    const float3 shade = lerp(skyHue * 0.62, sunHue * 1.15, wrap * wrap * (3.0 - 2.0 * wrap));
    return shade + sunHue * back * 0.35;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    // The disc's particle pages (128 px) are chunky pixel art drawn point-sampled on the
    // PS2: snap to texel centres so the shared linear sampler cannot blur them. Larger,
    // purpose-made effect atlases filter smoothly.
    uint width, height;
    g_textures[input.textureIndex].GetDimensions(width, height);
    const float2 texSize = float2(max(width, 1u), max(height, 1u));
    const float2 uv = width <= 128u ? (floor(input.uv * texSize) + 0.5) / texSize : input.uv;
    float4 texel = g_textures[input.textureIndex].Sample(g_linearSampler, uv);
    // Both blends weight the source by its alpha (additive is Cs*As + Cd), so an empty texel adds
    // nothing: skip the depth read and the blend for the bare corners of every quad.
    if (texel.a * input.color.a < 1.0 / 255.0)
    {
        discard;
    }
    texel.a *= SoftFade(input.position, input.viewPos, input.size);
    if (input.lit != 0u)
    {
        texel.rgb *= LitBall(input.ball);
    }
    if (input.displaySpace == 0u)
    {
        return texel * input.color; // linear, into the scene HDR colour
    }
    // Display space (drawn after the tonemap into the gamma-encoded LDR target): modulate
    // texel by colour in gamma space and clamp, as the PS2 GS does, so colours above 1
    // brighten dim page art and the blend below also happens in gamma space. 2.2 matches
    // the tonemap's own encode.
    const float3 gammaTexel = pow(max(texel.rgb, 0.0), 1.0 / 2.2);
    return float4(saturate(gammaTexel * input.color.rgb), texel.a * input.color.a);
}
