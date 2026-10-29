#include "../../src/shaders/include/LightTileFrustum.slangh"

struct LightData { float4 positionRadius, colorIntensity, directionType, coneShadow; };
struct CullData { float4 view0, view1, view2; float4 projection; uint4 dimensions; };
[[vk::binding(0, 0)]] ConstantBuffer<CullData> cull;
[[vk::binding(1, 0)]] StructuredBuffer<LightData> lights;
// Each tile has a count followed by 128 indices.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> tiles;
groupshared Atomic<uint> count;
groupshared uint indices[128];

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint lane = thread.x;
    if (lane == 0) count.store(0);
    GroupMemoryBarrierWithGroupSync();
    float2 lo = float2(group.xy * 16);
    float2 hi = min(lo + 16, float2(cull.dimensions.xy));
    float2 a = (lo / float2(cull.dimensions.xy) * 2 - 1) * cull.projection.yz;
    float2 b = (hi / float2(cull.dimensions.xy) * 2 - 1) * cull.projection.yz;
    LightTilePlanes planes = BuildLightTilePlanes(a.x, b.x, min(a.y, b.y), max(a.y, b.y), cull.projection.w != 0);
    for (uint i = lane; i < cull.dimensions.w; i += 64)
    {
        LightData light = lights[i];
        float4 p = float4(light.positionRadius.xyz, 1);
        float3 pos = float3(dot(cull.view0, p), dot(cull.view1, p), dot(cull.view2, p));
        float radius = max(light.positionRadius.w, 1e-4);
        if (-pos.z + radius < cull.projection.x || dot(planes.left, float4(pos, 1)) < -radius ||
            dot(planes.right, float4(pos, 1)) < -radius || dot(planes.bottom, float4(pos, 1)) < -radius ||
            dot(planes.top, float4(pos, 1)) < -radius) continue;
        if (light.directionType.w > 0.5 && light.coneShadow.y > 0.05)
        {
            float4 d = float4(light.directionType.xyz, 0);
            float3 direction = normalize(float3(dot(cull.view0, d), dot(cull.view1, d), dot(cull.view2, d)));
            float cosine = light.coneShadow.y;
            float tangent = sqrt(max(1 - cosine * cosine, 0)) / cosine;
            if (!LightTileIntersectsCone(planes, pos, direction, radius, tangent)) continue;
        }
        uint slot = count.add(1);
        if (slot < 128) indices[slot] = i;
    }
    GroupMemoryBarrierWithGroupSync();
    uint base = (group.y * cull.dimensions.z + group.x) * (min(cull.dimensions.w, 128) + 1);
    uint n = min(count.load(), 128);
    for (uint i = lane; i < n; i += 64) tiles[base + 1 + i] = indices[i];
    if (lane == 0) tiles[base] = n;
}
