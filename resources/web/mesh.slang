#include "../../src/shaders/include/PBRMath.slangh"
#include "../../src/shaders/include/OctNormal.slangh"
#include "../../src/shaders/include/MaterialLookMath.slangh"
#include "../../src/shaders/include/SceneVolumeMath.slangh"
#include "../../src/shaders/include/FoliageWindMath.slangh"
#include "../../src/shaders/include/DistanceFade.slangh"
#include "../../src/shaders/include/CloudMaterialMath.slangh"
#include "../../src/shaders/include/LightShaftMath.slangh"
#include "../../src/shaders/include/AmbientOcclusionMath.slangh"
#include "../../src/shaders/include/EnvBrdf.slangh"
#include "../../src/shaders/include/Sky.slangh"
#include "../../src/shaders/include/HeightFog.slangh"
#include "../../src/shaders/include/ShadowSampling.slangh"
#include "../../src/shaders/include/WaterMath.slangh"
#include "../../src/shaders/include/ContactShadowMath.slangh"
#include "../../src/shaders/include/ProceduralMaterialMath.slangh"
struct FrameData
{
	float4 clip0, clip1, clip2, clip3;
	float4 time;
	float4 camera;
	float4 sunDirectionIntensity, sunColor, ambient;
	float4 horizon, zenith, ground, skyParams, shadingParams;
	uint4 counts;
	float4 viewDepth;
	float4 cascadeSplits;
	float4 fogParams, objectAmbient, objectLight0Direction, objectLight0Color, objectLight1Direction, objectLight1Color;
	float4 invClip0, invClip1, invClip2, invClip3;
	uint4 volumeCounts;
	float4 blobs[64];
	float4 darkness[32];
	float4 visibilityCamera;
	float4 grassBenders[8];
	float4 grassBenderParams[8];
};
struct LightData { float4 positionRadius, colorIntensity, directionType, coneShadow; };
struct ShadowData { float4 c0, c1, c2, c3, params, lightPosRange, region; };
struct MeshData
{
	float4 world0, world1, world2, world3;
	float4 normal0, normal1, normal2;
	float4 color;
	float4 emissiveRoughness;
	float4 factors;
	float4 uvScroll;
	uint4 metadata;
	float4 distanceFade;
};
struct EffectData { float4 tint, params; };
struct SkinData { float4 c0, c1, c2, c3; };
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] StructuredBuffer<MeshData> meshes;
[[vk::binding(2, 0)]] StructuredBuffer<SkinData> skin;
[[vk::binding(3, 0)]] StructuredBuffer<LightData> lights;
[[vk::binding(4, 0)]] StructuredBuffer<ShadowData> shadows;
[[vk::binding(5, 0)]] Texture2DArray<float> shadowDepth;
[[vk::binding(6, 0)]] StructuredBuffer<EffectData> effects;
[[vk::binding(0, 1)]] Texture2D<float4> albedo;
[[vk::binding(1, 1)]] Texture2D<float4> emissive;
[[vk::binding(2, 1)]] SamplerState materialSampler;
[[vk::binding(3, 1)]] Texture2D<float4> normalMap;
[[vk::binding(4, 1)]] Texture2D<float4> metallicRoughness;
[[vk::binding(5, 1)]] Texture2D<float4> occlusion;
[[vk::binding(0, 2)]] Texture2D<float4> sceneDepth;
[[vk::binding(1, 2)]] Texture2D<float4> normalRoughness;
[[vk::binding(2, 2)]] Texture2D<float4> sceneAlbedo;
[[vk::binding(3, 2)]] Texture2D<float4> ambientOcclusion;
[[vk::binding(4, 2)]] SamplerState sceneSampler;
struct VertexOutput
{
	float4 position : SV_Position;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	nointerpolation uint instance : TEXCOORD1;
	float3 world : TEXCOORD2;
	float3 normal : TEXCOORD3;
	float4 tangent : TEXCOORD4;
	float3 rawvertexColor : TEXCOORD5;
	float2 rawUv : TEXCOORD6;
	float3 prepassNormal : TEXCOORD7;
	float2 sheet : TEXCOORD8;
};
[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION, float3 color : COLOR0, float2 uv : TEXCOORD0, uint4 joints : BLENDINDICES, float4 weights : BLENDWEIGHT, float3 normal : NORMAL, float4 tangent : TANGENT, float2 uv2 : TEXCOORD7, uint instance : SV_InstanceID)
{
	MeshData mesh = meshes[instance];
	float4 local = float4(position, 1);
	float3 localNormal = normal, localTangent = tangent.xyz;
	if (mesh.metadata.z != 0)
	{
		local = float4(0);
		localNormal = localTangent = float3(0);
		for (uint i = 0; i < 4; ++i)
		{
			if (weights[i] > 0 && joints[i] < mesh.metadata.z)
			{
				SkinData bone = skin[mesh.metadata.y + joints[i]];
				local += (bone.c0 * position.x + bone.c1 * position.y + bone.c2 * position.z + bone.c3) * weights[i];
				localNormal += (bone.c0.xyz * normal.x + bone.c1.xyz * normal.y + bone.c2.xyz * normal.z) * weights[i];
				localTangent += (bone.c0.xyz * tangent.x + bone.c1.xyz * tangent.y + bone.c2.xyz * tangent.z) * weights[i];
			}
		}
	}
	float4 world = mesh.world0 * local.x + mesh.world1 * local.y + mesh.world2 * local.z + mesh.world3 * local.w;
	if ((mesh.metadata.w & 32768) != 0)
		world.xyz += SharedGrassDisplacement(frame.time.x, saturate(frame.shadingParams.w), world.xyz, uv2.x, uv2.y * length(mesh.world1.xyz), mesh.distanceFade.z, frame.grassBenders, frame.grassBenderParams);
	else
		world.xyz += SharedFoliageWindOffset(saturate(frame.shadingParams.w), mesh.metadata.w, frame.time.x, world.xyz, uv2.x);
	if ((mesh.metadata.w & 512) != 0) { world.xyz += frame.camera.xyz - mesh.world3.xyz; }
	VertexOutput output;
	output.position = float4(dot(frame.clip0, world), dot(frame.clip1, world), dot(frame.clip2, world), dot(frame.clip3, world));
	output.position.y = -output.position.y;
	if ((mesh.metadata.w & 512) != 0) { output.position.z = output.position.w; }
	float3 vertexColor = (mesh.metadata.w & 8) != 0 ? color : float3(1);
	if (frame.time.y != 0 && (any(vertexColor < float3(0)) || any(vertexColor > float3(1)) || dot(vertexColor, vertexColor) < 0.0025)) { vertexColor = float3(1); }
	output.color = mesh.color * float4(vertexColor, 1);
	output.uv = uv + frac(mesh.uvScroll.xy * frame.time.x);
	if ((mesh.metadata.w & 512) != 0 && mesh.factors.x > 0 && (uint(mesh.factors.w) & 1) != 0)
	{
		output.uv += frac(float2(0.0015, 0) * (frame.time.x * saturate(frame.shadingParams.w)));
	}
	output.rawvertexColor = color;
	output.rawUv = uv;
	output.sheet = uv2;
	output.prepassNormal = mesh.world0.xyz * localNormal.x + mesh.world1.xyz * localNormal.y + mesh.world2.xyz * localNormal.z;
	output.instance = instance;
	output.world = world.xyz;
	output.normal = mesh.normal0.xyz * localNormal.x + mesh.normal1.xyz * localNormal.y + mesh.normal2.xyz * localNormal.z;
	output.tangent = float4(mesh.world0.xyz * localTangent.x + mesh.world1.xyz * localTangent.y + mesh.world2.xyz * localTangent.z, tangent.w * ((mesh.metadata.w & 0x80000000) != 0 ? -1 : 1));
	return output;
}
float4 ProjectWorld(float3 world)
{
	float4 p = float4(world, 1);
	return float4(dot(frame.clip0, p), dot(frame.clip1, p), dot(frame.clip2, p), dot(frame.clip3, p));
}
float3 UnprojectWorld(float3 ndc)
{
	float4 p = float4(ndc, 1);
	float4 world = float4(dot(frame.invClip0, p), dot(frame.invClip1, p), dot(frame.invClip2, p), dot(frame.invClip3, p));
	return world.xyz / max(world.w, 1e-6);
}
float LightShaftCoverage(VertexOutput input)
{
	uint width, height;
	sceneDepth.GetDimensions(width, height);
	float soft = 1;
	float4 clip = ProjectWorld(input.world);
	if (clip.w > 0)
	{
		float3 ndc = clip.xyz / clip.w;
		int2 texel = int2(saturate(ndc.xy * 0.5 + 0.5) * float2(width - 1, height - 1));
		float depth = sceneDepth.Load(int3(texel, 0)).r;
		soft = saturate((distance(frame.camera.xyz, UnprojectWorld(float3(ndc.xy, depth))) - distance(frame.camera.xyz, input.world)) / 1.5);
	}
	return SharedLightShaftFade(frame.camera.xyz, frame.time.x, soft, input.rawUv, input.sheet, input.world, input.normal);
}
float3 EncodeSrgb(float3 value)
{
	value = max(value, float3(0));
	return select(value <= float3(0.0031308), value * 12.92, 1.055 * pow(value, float3(1.0 / 2.4)) - 0.055);
}
float3 SafeNormal(float3 value, float3 fallback)
{
	return dot(value, value) > 1e-12 ? normalize(value) : fallback;
}
float3 Brdf(float3 N, float3 V, float3 L, float3 base, float metallic, float roughness, float specularWeight)
{
	float3 H = SafeNormal(V + L, N);
	float nl = saturate(dot(N, L)), nv = max(saturate(dot(N, V)), 1e-4);
	float3 F = F_Schlick(saturate(dot(V, H)), lerp(float3(0.04), base, metallic));
	float3 specular = D_GGX(saturate(dot(N, H)), roughness) * G_SmithSchlick(nv, nl, roughness) * F / max(4 * nl * nv, 1e-4);
	return ((1 - F) * (1 - metallic) * base / kPI + specular * specularWeight) * nl;
}
float LoadShadow(uint layer, float2 uv)
{
	uint width, height, layers;
	shadowDepth.GetDimensions(width, height, layers);
	float4 region = shadows[layer].region;
	int2 texel = clamp(int2(floor(region.xy + uv * region.zw)), int2(region.xy), int2(region.xy + region.zw - 1));
	return shadowDepth.Load(int4(texel, 0, 0));
}
float ShadowVisibility(uint layer, float3 world, float3 N, float3 L, float2 pixel)
{
	ShadowData data = shadows[layer];
	float nl = saturate(dot(N, L));
	if (nl <= 0) { return 1; }
	if (data.params.w != 0)
	{
		float range = max(data.lightPosRange.w, 1e-3);
		float dist = distance(world, data.lightPosRange.xyz);
		float texelWorld = max(dist * data.params.y, 1e-5);
		float3 samplePos = world + N * (1.5 * texelWorld * sqrt(saturate(1 - nl * nl)));
		float4 clip = data.c0 * samplePos.x + data.c1 * samplePos.y + data.c2 * samplePos.z + data.c3;
		float3 projected = clip.xyz / clip.w;
		if (clip.w <= 0 || any(abs(projected.xy) > float2(1)) || projected.z < 0 || projected.z > 1) { return 1; }
		float2 localUv = projected.xy * 0.5 + 0.5;
		float receiver = saturate((dist - texelWorld) / range);
		if (receiver <= 0) { return 1; }
		float sourceRadius = max(data.params.z, 1e-4) / range;
		float search = sourceRadius / max(receiver, 1e-3);
		float phi = InterleavedGradientNoise(pixel) * kTwoPi;
		float blocker = 0, count = 0;
		for (uint i = 0; i < 6; ++i)
		{
			float d = LoadShadow(layer, saturate(localUv + VogelDiskSample(i, 6, phi) * search));
			if (d < receiver) { blocker += d; count += 1; }
		}
		if (count <= 0) { return 1; }
		float avg = blocker / count;
		float radius = (receiver - avg) / max(avg, 1e-4) * sourceRadius / max(receiver, 1e-3);
		float texel = 1.0 / min(data.region.z, data.region.w);
		radius = clamp(radius, texel, max(search, texel));
		float visibility = 0;
		for (uint i = 0; i < 8; ++i) { visibility += receiver <= LoadShadow(layer, saturate(localUv + VogelDiskSample(i, 8, phi) * radius)) ? 1 : 0; }
		return visibility / 8;
	}
	world += N * (data.params.y * sqrt(saturate(1 - nl * nl)));
	float4 p = data.c0 * world.x + data.c1 * world.y + data.c2 * world.z + data.c3;
	float3 ndc = p.xyz / p.w;
	float2 uv = ndc.xy * 0.5 + 0.5;
	if (p.w <= 0 || any(uv <= float2(0)) || any(uv >= float2(1)) || ndc.z <= 0 || ndc.z >= 1) { return 1; }
	uint width, height, layers;
	shadowDepth.GetDimensions(width, height, layers);
	float maxTexel = 1.0 / min(data.region.z, data.region.w);
	float sinTheta = sqrt(saturate(1 - nl * nl));
	float slope = min(sinTheta / max(nl, 1e-3), 4);
	float slope0 = data.params.x * 0.70710678 * 0.71 * slope;
	float slopeBase = data.params.x * 0.70710678 * slope * 0.71;
	float slopePerUv = data.params.x * 0.70710678 * slope / maxTexel;
	float searchRadius = min(data.params.z, 6 * maxTexel);
	float phi = InterleavedGradientNoise(pixel) * kTwoPi;
	float blockerSum = 0, blockerCount = 0;
	for (uint i = 0; i < 6; ++i)
	{
		float2 offset = VogelDiskSample(i, 6, phi) * searchRadius;
		float d = LoadShadow(layer, uv + offset);
		float bias = data.params.x + slope0 + slopeBase + slopePerUv * length(offset);
		if (d < ndc.z - bias)
		{
			blockerSum += d;
			blockerCount += 1;
		}
	}
	if (blockerCount <= 0) { return 1; }
	float radius = clamp((ndc.z - blockerSum / blockerCount) * data.params.z, maxTexel, max(searchRadius, maxTexel));
	float visible = 0;
	for (uint i = 0; i < 8; ++i)
	{
		float2 offset = VogelDiskSample(i, 8, phi) * radius;
		float bias = data.params.x + slope0 + slopeBase + slopePerUv * length(offset);
		visible += ndc.z - bias <= LoadShadow(layer, uv + offset) ? 1 : 0;
	}
	return visible / 8;
}
uint PointFace(float3 delta)
{
	float3 d = abs(delta);
	if (d.x >= d.y && d.x >= d.z) { return delta.x >= 0 ? 0 : 1; }
	if (d.y >= d.z) { return delta.y >= 0 ? 2 : 3; }
	return delta.z >= 0 ? 4 : 5;
}
[shader("fragment")]
float shadowMain(VertexOutput input) : SV_Target
{
	MeshData mesh = meshes[input.instance];
	float alpha = input.color.a * albedo.Sample(materialSampler, input.uv).a;
	if ((mesh.metadata.w & 4) != 0 && alpha < mesh.factors.z) { discard; }
	if (DistanceFadeDiscard(mesh.distanceFade.xy, input.world, frame.visibilityCamera.xyz, input.position.xy)) { discard; }
	return frame.camera.w > 0 ? saturate(distance(input.world, frame.camera.xyz) / frame.camera.w) : input.position.z;
}
float4 ShadeProcedural(VertexOutput input, EffectData data)
{
	EffectParams fx;
	fx.tint = data.tint; fx.speed = data.params.x; fx.scale = data.params.y; fx.intensity = data.params.z; fx._pad = 0;
	if (data.params.w == 1) { return SharedPlasmaShade(input.normal, input.tangent.xyz, input.tangent.w, input.sheet, input.world, frame.camera.xyz, frame.time.x, fx); }
	return SharedMoltenShade(input.normal, input.tangent.xyz, input.tangent.w, input.sheet, input.world, frame.camera.xyz, frame.time.x, fx);
}
struct AuxiliaryOutput
{
	float depth : SV_Target0;
	float4 surface : SV_Target1;
	float4 baseColor : SV_Target2;
};
#ifdef AE_WEB_CUSTOM_MATERIAL
#include "aether-web-material.slangh"
#endif
[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target
{
#ifdef AE_WEB_CUSTOM_MATERIAL
	float4 graphColor = ShadeGraphMaterial(input, front);
	if (DistanceFadeDiscard(meshes[input.instance].distanceFade.xy, input.world, frame.visibilityCamera.xyz, input.position.xy)) { discard; }
	return graphColor;
#endif
	MeshData mesh = meshes[input.instance];
	float2 uvDx = ddx(input.uv), uvDy = ddy(input.uv);
	float3 faceN = cross(ddx(input.world), ddy(input.world));
	float4 surface = input.color * albedo.SampleGrad(materialSampler, input.uv, uvDx, uvDy);
	float3 cloudShade = SharedCloudShade(albedo, materialSampler, input.uv, surface.a / max(input.color.a, 1e-6), input.world, frame.camera.xyz, normalize(frame.sunDirectionIntensity.xyz));
	if ((mesh.metadata.w & 512) != 0 && mesh.factors.x > 0 && (uint(mesh.factors.w) & 1) != 0)
	{
		surface.rgb *= lerp(float3(1), cloudShade, saturate(mesh.factors.x) * saturate(frame.shadingParams.w));
	}
	if ((mesh.metadata.w & 1024) != 0) { surface.a *= LightShaftCoverage(input); }
	float3 emission = mesh.emissiveRoughness.xyz * emissive.SampleGrad(materialSampler, input.uv, uvDx, uvDy).xyz;
	MaterialLookDerivatives lookDerivatives;
	lookDerivatives.worldDx = ddx(input.world); lookDerivatives.worldDy = ddy(input.world);
	lookDerivatives.uvDx = uvDx; lookDerivatives.uvDy = uvDy;
	float luminance = dot(surface.rgb, float3(0.299, 0.587, 0.114));
	lookDerivatives.luminanceDx = ddx(luminance); lookDerivatives.luminanceDy = ddy(luminance);
	float3 tangentNormal = normalMap.SampleGrad(materialSampler, input.uv, uvDx, uvDy).xyz * 2 - 1;
	float alphaWidth = fwidth(surface.a);
	float edgeCoverage = 1;
	float3 averageAlbedo = albedo.SampleLevel(materialSampler, input.uv, 12).rgb * input.color.rgb;

	if (frame.time.y != 0)
	{
		float3 N = dot(input.normal, input.normal) > 1e-12 ? normalize(input.normal) * (front ? 1 : -1) : SafeNormal(faceN, float3(0, 1, 0));
		float3 Ngeom = N;
		float3 T = SafeNormal(input.tangent.xyz, float3(1, 0, 0));
		T -= N * dot(T, N);
		float3 fallbackT = cross(float3(0, 1, 0), N);
		if (dot(fallbackT, fallbackT) < 1e-12) { fallbackT = cross(float3(1, 0, 0), N); }
		T = SafeNormal(T, SafeNormal(fallbackT, float3(1, 0, 0)));
		float3 B = cross(N, T) * input.tangent.w;
		float variance = 0;
		if ((uint(mesh.factors.w) & 2) != 0)
		{
			float3 tn = tangentNormal;
			float len = saturate(length(tn));
			variance = max(0, 1 - len - 0.04) / max(len, 1e-3);
			N = SafeNormal(lerp(N, SafeNormal(T * tn.x + B * tn.y + N * tn.z, N), 0.60), N);
		}
		float3 orm = metallicRoughness.SampleGrad(materialSampler, input.uv, uvDx, uvDy).xyz;
		float roughness = clamp(mesh.emissiveRoughness.w * orm.g, 0.04, 1);
		float metallic = saturate(mesh.factors.x * orm.b);
		float3 dx = ddx(N), dy = ddy(N);
		roughness = sqrt(saturate(roughness * roughness + min(2 * 0.15915494 * (dot(dx, dx) + dot(dy, dy)) + variance * 0.60, 0.18) * saturate(frame.shadingParams.x)));
		if ((mesh.metadata.w & 0x40000000) != 0)
		{
			float band = min(alphaWidth, 0.35) * saturate(frame.shadingParams.w);
			if (band <= 1e-4 || surface.a >= mesh.factors.z || surface.a < mesh.factors.z - band) { discard; }
			edgeCoverage = saturate((surface.a - (mesh.factors.z - band)) / band);
		}
		else if ((mesh.metadata.w & 4) != 0 && surface.a < mesh.factors.z) { discard; }
		uint depthWidth, depthHeight;
		sceneDepth.GetDimensions(depthWidth, depthHeight);
		float screenAo = saturate(ambientOcclusion.SampleLevel(sceneSampler, saturate(input.position.xy / float2(depthWidth, depthHeight)), 0).r);
		screenAo = (mesh.metadata.w & (2 | 4 | 128)) == 0 ? screenAo : 1;
		float ao = saturate(lerp(1, occlusion.SampleGrad(materialSampler, input.uv, uvDx, uvDy).r, mesh.factors.y) * screenAo);
		float3 V = SafeNormal(frame.camera.xyz - input.world, N);
		float3 L = SafeNormal(frame.sunDirectionIntensity.xyz, float3(0, 1, 0));
		float3 lit = Brdf(N, V, L, surface.rgb, metallic, roughness, 1) * frame.sunColor.rgb * frame.sunDirectionIntensity.w;
		float sunVisibility = 1;
		bool receives = (mesh.metadata.w & 16) == 0;
		if (receives && frame.counts.y != 0)
		{
			float depth = dot(frame.viewDepth, float4(input.world, 1));
			uint layer = depth <= frame.cascadeSplits.x ? 0 : (depth <= frame.cascadeSplits.y ? 1 : 2);
			float visibility = ShadowVisibility(layer, input.world, Ngeom, L, input.position.xy);
			if (layer < 2)
			{
				float splitFar = frame.cascadeSplits[layer], splitNear = layer == 0 ? 0 : frame.cascadeSplits[layer - 1];
				float t = smoothstep(splitFar - max((splitFar - splitNear) * 0.15, 2), splitFar, depth);
				if (t > 0) { visibility = lerp(visibility, ShadowVisibility(layer + 1, input.world, Ngeom, L, input.position.xy), t); }
			}
			visibility = lerp(visibility, 1, smoothstep(frame.cascadeSplits.z * 0.85, frame.cascadeSplits.z, depth));
			sunVisibility = visibility;
			// Native retains an 8 percent diffuse floor, while blocked specular vanishes.
			float3 sun = frame.sunColor.rgb * frame.sunDirectionIntensity.w;
			float3 diffuseOnly = Brdf(N, V, L, surface.rgb, metallic, roughness, 0) * sun;
			lit = lit * visibility + diffuseOnly * (max(visibility, 0.08) - visibility);
		}
		float contact = frame.counts.w != 0 && receives && (mesh.metadata.w & 32) == 0
			? SharedContactShadow(input.world, Ngeom, L, float4x4(frame.clip0, frame.clip1, frame.clip2, frame.clip3), float4x4(frame.invClip0, frame.invClip1, frame.invClip2, frame.invClip3), sceneDepth, input.position.xy) : 1;
		float cloudShadow = receives ? CloudSunVisibility(input.world, L, frame.shadingParams.y, frame.shadingParams.z, frame.time.x) : 1;
		sunVisibility = receives && frame.counts.y != 0 ? min(min(sunVisibility, contact), cloudShadow) : 1;
		float3 sunRadiance = frame.sunColor.rgb * frame.sunDirectionIntensity.w;
		lit = (Brdf(N, V, L, surface.rgb, metallic, roughness, 1) * sunVisibility
			+ Brdf(N, V, L, surface.rgb, metallic, roughness, 0) * (max(sunVisibility, 0.08) - sunVisibility)) * sunRadiance;
		for (uint i = 0; i < frame.counts.x; ++i)
		{
			float3 Nlocal = SafeNormal(lerp(Ngeom, N, 0.35), Ngeom);
			LightData light = lights[i];
			float3 delta = light.positionRadius.xyz - input.world;
			float dist2 = dot(delta, delta), radius2 = light.positionRadius.w * light.positionRadius.w;
			float3 direction = delta / max(sqrt(max(dist2, 1e-8)), 1e-4);
			float window = saturate(1 - dist2 * dist2 / max(radius2 * radius2, 1e-6));
			float attenuation = window * window / max(dist2, 0.01);
			if (attenuation <= 1e-5 || dot(Nlocal, direction) <= 1e-5) { continue; }
			if (light.directionType.w > 0)
			{
				float spot = saturate((dot(light.directionType.xyz, -direction) - light.coneShadow.y) / max(light.coneShadow.x - light.coneShadow.y, 1e-5));
				if (spot <= 1e-5) { continue; }
				attenuation *= spot;
			}
			if (receives && light.coneShadow.z >= 0)
			{
				uint layer = uint(light.coneShadow.z) + (light.directionType.w == 0 ? PointFace(-delta) : 0);
				attenuation *= ShadowVisibility(layer, input.world, Nlocal, direction, input.position.xy);
			}
			lit += Brdf(Nlocal, V, direction, surface.rgb, metallic, roughness, 0.28) * light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
		}
		float nv = max(saturate(dot(N, V)), 1e-4);
		float3 F0 = lerp(float3(0.04), surface.rgb, metallic);
		float3 F = F_Schlick(saturate(dot(V, SafeNormal(V + L, N))), F0);
		float3 skyDiffuse = SkyGradient(N, L, frame.sunColor.rgb, frame.sunDirectionIntensity.w, frame.horizon.rgb, frame.zenith.rgb, frame.ground.rgb, frame.skyParams, frame.shadingParams.y, 1);
		float3 R = SafeNormal(lerp(reflect(-V, N), N, roughness * roughness), N);
		float3 skySpecular = SkySpecularProbe(R, input.world, L, frame.sunColor.rgb, frame.sunDirectionIntensity.w, frame.horizon.rgb, frame.zenith.rgb, frame.ground.rgb, frame.skyParams, frame.shadingParams.y, frame.shadingParams.z, frame.time.x, roughness);
		float3 ambient = (frame.ambient.rgb + skyDiffuse) * surface.rgb * (1 - F) * (1 - metallic) * MultiBounceAo(ao, surface.rgb)
			+ skySpecular * EnvSpecularWeight(F0, EnvBrdfApprox(roughness, nv)) * SpecularOcclusion(ao, nv, roughness);
		float3 finalColor = max(lit + ambient + emission, float3(0));
		MaterialLookFrame look;
		look.cameraWorldPos = frame.camera;
		look.objectAmbient = frame.objectAmbient;
		look.objectLight0Direction = frame.objectLight0Direction;
		look.objectLight0Color = frame.objectLight0Color;
		look.objectLight1Direction = frame.objectLight1Direction;
		look.objectLight1Color = frame.objectLight1Color;
		look.skyHorizonColor = frame.horizon;
		look.skyZenithColor = frame.zenith;
		if ((mesh.metadata.w & 1024) != 0)
		{
			finalColor = surface.rgb + emission;
		}
		else if ((mesh.metadata.w & 2048) != 0)
		{
			float coverage;
			finalColor = SharedGemShade(look, surface.rgb, averageAlbedo, input.world, mesh.emissiveRoughness.w,
				emission, sunVisibility, coverage, lookDerivatives);
			surface.a = max(surface.a, coverage);
		}
		else if ((mesh.metadata.w & 8192) != 0)
		{
			WaterFrame water;
			water.viewProj = float4x4(frame.clip0, frame.clip1, frame.clip2, frame.clip3);
			water.invViewProj = float4x4(frame.invClip0, frame.invClip1, frame.invClip2, frame.invClip3);
			water.cameraWorldPos = frame.camera; water.sunDirectionIntensity = frame.sunDirectionIntensity; water.sunColor = frame.sunColor;
			water.skyHorizonColor = frame.horizon; water.skyZenithColor = frame.zenith; water.skyVoidColor = frame.ground;
			water.skyParams = frame.skyParams; water.shadingParams = frame.shadingParams; water.shadowParams = float4(0, 0, 1, frame.counts.w);
			water.elapsedTime = frame.time.x; water.sceneDepthValid = true; water.sceneColorValid = true;
			WaterMaterial material;
			material.baseColorFactor = mesh.color; material.emissiveFactor = float4(mesh.emissiveRoughness.xyz, 0);
			material.uvScroll = mesh.uvScroll.xy; material.roughnessFactor = mesh.emissiveRoughness.w; material.metallicFactor = mesh.factors.x;
			material.flags = mesh.metadata.w; material.albedoSlot = (uint(mesh.factors.w) & 1) != 0 ? 0 : 0xFFFFFFFFu;
			WaterDerivatives derivatives;
			derivatives.worldDx = lookDerivatives.worldDx; derivatives.worldDy = lookDerivatives.worldDy;
			derivatives.uvDx = lookDerivatives.uvDx; derivatives.uvDy = lookDerivatives.uvDy;
			float3 vertexColor = (mesh.metadata.w & 8) != 0 ? input.rawvertexColor : float3(1);
			if (any(vertexColor < float3(0)) || any(vertexColor > float3(1)) || dot(vertexColor, vertexColor) < 0.0025) { vertexColor = float3(1); }
			float coverage;
			finalColor = SharedWaterShade(water, material, albedo, materialSampler, sceneDepth, sceneAlbedo, derivatives, surface.rgb, surface.a, input.uv, input.rawUv,
				input.world, Ngeom, input.position.xy, vertexColor * mesh.color.rgb, sunVisibility, coverage);
			surface.a = coverage;
		}
		else if ((mesh.metadata.w & 64) != 0)
		{
			float3 vertexColor = (mesh.metadata.w & 8) != 0 ? input.rawvertexColor : float3(1);
			if (any(vertexColor < float3(0)) || any(vertexColor > float3(1)) || dot(vertexColor, vertexColor) < 0.0025) { vertexColor = float3(1); }
			float3 bakedLight = max(vertexColor * mesh.color.rgb, float3(1e-4));
			float3 skyShade = SkyGradient(Ngeom, L, frame.sunColor.rgb, frame.sunDirectionIntensity.w, frame.horizon.rgb, frame.zenith.rgb, frame.ground.rgb, frame.skyParams, frame.shadingParams.y, 1);
			float3 shadeLight = min(bakedLight, frame.ambient.rgb + skyShade);
			float facing = (mesh.metadata.w & 32) != 0 ? 1 : smoothstep(0, 0.1, dot(Ngeom, L));
			float bakedVisibility = frame.counts.y != 0 ? sunVisibility * facing : 1;
			float3 bakedLit = shadeLight * ao + (bakedLight - shadeLight) * bakedVisibility;
			finalColor = surface.rgb * (bakedLit / bakedLight) * SharedBlobShadowVisibility(frame.volumeCounts.xy, frame.blobs, input.world, (mesh.metadata.w & 32) != 0 ? 1 : saturate(Ngeom.y * 2)) + emission;
			if ((mesh.metadata.w & (32 | 64 | 512)) == (32 | 64))
			{
				float back = saturate(-dot(Ngeom, L)), into = saturate(dot(-V, L));
				float peak = max(max(surface.r, surface.g), max(surface.b, 1e-4));
				float3 glow = surface.rgb * lerp(float3(1), surface.rgb / peak, 0.6);
				finalColor += glow * (0.6 * back * (0.4 + 0.6 * into * into) * bakedVisibility * saturate(frame.shadingParams.w));
			}
		}
		else if ((mesh.metadata.w & 128) != 0)
		{
			float3 Nobj = Ngeom;
			if ((uint(mesh.factors.w) & 2) != 0) { Nobj = SharedObjectLitNormal(Ngeom, input.world, input.uv, tangentNormal, lookDerivatives); }
			float3 objectLight = frame.objectAmbient.rgb * ao
				+ frame.objectLight0Color.rgb * (saturate(dot(Nobj, frame.objectLight0Direction.xyz)) * sunVisibility)
				+ frame.objectLight1Color.rgb * saturate(dot(Nobj, frame.objectLight1Direction.xyz));
			finalColor = surface.rgb * pow(max(objectLight, float3(0)), float3(2.2));
			if ((uint(mesh.factors.w) & 4) != 0)
			{
				float3 key = pow(max(frame.objectLight0Color.rgb, float3(0)), float3(2.2)) * sunVisibility;
				float3 fill = pow(max(frame.objectLight1Color.rgb, float3(0)), float3(2.2));
				finalColor *= 1 - 0.35 * metallic;
				finalColor += ObjectLitSpecular(Nobj, V, frame.objectLight0Direction.xyz, roughness, F0) * key
					+ ObjectLitSpecular(Nobj, V, frame.objectLight1Direction.xyz, roughness, F0) * fill;
				float2 env = EnvBrdfApprox(roughness, saturate(dot(Nobj, V)));
				float3 sky = SkyGradient(reflect(-V, Nobj), L, frame.sunColor.rgb, frame.sunDirectionIntensity.w, frame.horizon.rgb, frame.zenith.rgb, frame.ground.rgb, frame.skyParams, frame.shadingParams.y, roughness);
				finalColor += sky * (F0 * env.x + env.y) * (ao * 0.75);
			}
			finalColor += emission;
			if ((mesh.metadata.w & 16384) != 0)
			{
				finalColor = max(finalColor + SharedCharacterLook(look, surface.rgb, surface.a, mesh.factors.z, (mesh.metadata.w & 4) != 0,
					Nobj, V, input.world, input.uv, roughness, emission, sunVisibility, ao, lookDerivatives), float3(0));
			}
			if ((mesh.metadata.w & 4096) != 0)
			{
				float3 key = pow(max(frame.objectLight0Color.rgb, float3(0)), float3(2.2)) * sunVisibility;
				finalColor += key * lerp(surface.rgb, float3(1), 0.6) * (PickupHighlight(N, V, frame.objectLight0Direction.xyz, roughness) * 0.2);
			}
		}
		if ((mesh.metadata.w & 32768) != 0)
			finalColor += SharedGrassBacklight(surface.rgb, V, L, frame.sunColor.rgb, frame.sunDirectionIntensity.w, sunVisibility);
		float fogAmount = frame.skyParams.z > 0 ? 0 : HeightFogAmount(frame.camera.xyz, input.world, frame.fogParams.x, frame.fogParams.y, frame.fogParams.w);
		if (frame.skyParams.z <= 0 && (mesh.metadata.w & (512 | 1024 | 256)) == 0)
		{
			float aerial = 0.22 * saturate(frame.shadingParams.w) * smoothstep(50, 450, length(input.world - frame.camera.xyz));
			finalColor = lerp(finalColor, float3(0.36, 0.47, 0.92), max(aerial - fogAmount, 0));
		}
		if (fogAmount > 0)
		{
			float3 fog = HeightFogColor(SafeNormal(input.world - frame.camera.xyz, N), L, frame.sunColor.rgb, frame.sunDirectionIntensity.w,
				frame.horizon.rgb, frame.zenith.rgb, frame.ground.rgb, frame.fogParams.z, frame.skyParams, frame.shadingParams.y);
			finalColor = lerp(finalColor, fog, fogAmount);
		}
		finalColor *= 1 - SharedDarknessVolumeAmount(frame.volumeCounts.z, frame.darkness, frame.camera.xyz, input.world);
		EffectData effect = effects[mesh.metadata.x];
		if (DistanceFadeDiscard(mesh.distanceFade.xy, input.world, frame.visibilityCamera.xyz, input.position.xy)) { discard; }
		if (effect.params.w != 0) { return ShadeProcedural(input, effect); }
		return float4(finalColor, (mesh.metadata.w & 2) != 0 ? saturate(surface.a) : edgeCoverage);

	}
	if ((mesh.metadata.w & 4) != 0 && surface.a < mesh.factors.z) { discard; }
	if (DistanceFadeDiscard(mesh.distanceFade.xy, input.world, frame.visibilityCamera.xyz, input.position.xy)) { discard; }
	EffectData effect = effects[mesh.metadata.x];
	if (effect.params.w != 0) { return ShadeProcedural(input, effect); }
	return float4(EncodeSrgb(surface.rgb + emission), surface.a);
}

[shader("fragment")]
AuxiliaryOutput auxiliaryMain(VertexOutput input, bool front : SV_IsFrontFace)
{
#ifdef AE_WEB_CUSTOM_MATERIAL
	AuxiliaryOutput graphOutput = ShadeGraphAuxiliary(input, front);
	if (DistanceFadeDiscard(meshes[input.instance].distanceFade.xy, input.world, frame.visibilityCamera.xyz, input.position.xy)) { discard; }
	return graphOutput;
#endif
	MeshData mesh = meshes[input.instance];
	float2 uvDx = ddx(input.rawUv), uvDy = ddy(input.rawUv);
	float4 base = albedo.SampleGrad(materialSampler, input.rawUv, uvDx, uvDy);
	float alpha = mesh.color.a * albedo.SampleGrad(materialSampler, input.uv, uvDx, uvDy).a;
	float3 orm = metallicRoughness.SampleGrad(materialSampler, input.rawUv, uvDx, uvDy).rgb;
	float3 N = SafeNormal(input.prepassNormal, float3(0, 1, 0)) * (front ? 1 : -1);
	float3 dx = ddx(N), dy = ddy(N);
	float roughness = clamp(mesh.emissiveRoughness.w * orm.g, 0.04, 1);
	roughness = sqrt(saturate(roughness * roughness + min(2 * 0.15915494 * (dot(dx, dx) + dot(dy, dy)), 0.18)));
	float3 vertexColor = (mesh.metadata.w & 8) != 0 ? input.rawvertexColor : float3(1);
	if (any(vertexColor < float3(0)) || any(vertexColor > float3(1)) || dot(vertexColor, vertexColor) < 0.0025) { vertexColor = float3(1); }
	if ((mesh.metadata.w & (2 | 512)) != 0 || ((mesh.metadata.w & 4) != 0 && alpha < mesh.factors.z)) { discard; }
	if (DistanceFadeDiscard(mesh.distanceFade.xy, input.world, frame.visibilityCamera.xyz, input.position.xy)) { discard; }
	AuxiliaryOutput output;
	output.depth = input.position.z;
	output.surface = float4(EncodeOctNormal(N), saturate(roughness), saturate(mesh.factors.x * orm.b));
	output.baseColor = float4(saturate(mesh.color.rgb * base.rgb * vertexColor), 1);
	return output;
}
