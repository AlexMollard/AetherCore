#include "../../src/shaders/include/ParticleBillboard.slangh"
#include "../../src/shaders/include/ParticleLightingMath.slangh"
struct BillboardFrame
{
	float4 view0, view1, view2, view3;
	float4 proj0, proj1, proj2, proj3;
	float4 inv0, inv1, inv2, inv3;
	float4 camera, sunDirectionIntensity, sunColor, zenith, extent;
};
[[vk::binding(0, 0)]] ConstantBuffer<BillboardFrame> frame;
[[vk::binding(1, 0)]] StructuredBuffer<GpuBillboardParticle> particles;
[[vk::binding(2, 0)]] Texture2D<float4> sceneDepth;
[[vk::binding(0, 1)]] Texture2D<float4> particleTexture;
[[vk::binding(1, 1)]] SamplerState particleSampler;
float4 View(float4 p) { return float4(dot(frame.view0, p), dot(frame.view1, p), dot(frame.view2, p), dot(frame.view3, p)); }
float4 Project(float4 p) { return float4(dot(frame.proj0, p), dot(frame.proj1, p), dot(frame.proj2, p), dot(frame.proj3, p)); }
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
    const GpuBillboardParticle p = particles[instanceId];
    const float2 corner = corners[vertexId % 6u];

    // Centre in view space, expand on the view plane there, then project. Working in view
    // space makes the billboarding exact without depending on the view matrix's storage
    // layout: +x is screen right and +y is screen up by definition.
    const float3 centreView = View(float4(p.positionSize.xyz, 1.0)).xyz;
    float2 local = (corner - 0.5) * p.positionSize.w;
    const float radians = p.rotationDegrees * 3.14159265 / 180.0;
    const float s = sin(radians);
    const float c = cos(radians);
    local = float2(local.x * c - local.y * s, local.x * s + local.y * c);
    const float4 clip = Project(float4(centreView + float3(local, 0.0), 1.0));

    VSOutput output;
    output.position = clip;
	output.position.y = -output.position.y;
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

float SoftFade(float4 svPosition, float3 viewPos, float size)
{
	if (frame.extent.z == 0) { return 1; }
	uint width, height;
	sceneDepth.GetDimensions(width, height);
	float2 uv = svPosition.xy / frame.extent.xy;
	int2 texel = clamp(int2(uv * float2(width, height)), int2(0), int2(width, height) - 1);
	float depth = sceneDepth.Load(int3(texel, 0)).r;
	if (svPosition.z > depth) { discard; }
	float2 ndc = (float2(texel) + 0.5) / float2(width, height) * 2 - 1;
	float4 clip = float4(ndc, depth, 1);
	float4 world = float4(dot(frame.inv0, clip), dot(frame.inv1, clip), dot(frame.inv2, clip), dot(frame.inv3, clip));
	world.xyz /= max(world.w, 1e-6);
	float range = clamp(size * 0.25, 0.02, 0.6);
	return saturate((distance(frame.camera.xyz, world.xyz) - length(viewPos)) / range);
}
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    // The disc's particle pages (128 px) are chunky pixel art drawn point-sampled on the
    // PS2: snap to texel centres so the shared linear sampler cannot blur them. Larger,
    // purpose-made effect atlases filter smoothly.
    uint width, height;
    particleTexture.GetDimensions(width, height);
    const float2 texSize = float2(max(width, 1u), max(height, 1u));
    const float2 uv = width <= 128u ? (floor(input.uv * texSize) + 0.5) / texSize : input.uv;
    float4 texel = particleTexture.SampleGrad(particleSampler, uv, ddx(uv) * exp2(frame.extent.w), ddy(uv) * exp2(frame.extent.w));
	// Particle pages share the sprite registry's encoded RGBA8 storage. Native
	// textures decode sRGB on sampling before either HDR lighting or display gamma.
	texel.rgb = select(texel.rgb <= float3(0.04045), texel.rgb / 12.92, pow((texel.rgb + 0.055) / 1.055, float3(2.4)));
    // Both blends weight the source by its alpha (additive is Cs*As + Cd), so an empty texel adds
    // nothing: skip the depth read and the blend for the bare corners of every quad.
    if (texel.a * input.color.a < 1.0 / 255.0)
    {
        discard;
    }
    texel.a *= SoftFade(input.position, input.viewPos, input.size);
    if (input.lit != 0u)
    {
        texel.rgb *= SharedParticleLitBall(input.ball, normalize(View(float4(normalize(frame.sunDirectionIntensity.xyz), 0)).xyz), frame.sunColor.rgb, frame.zenith.rgb);
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
