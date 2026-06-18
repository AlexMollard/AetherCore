// GPU tiled-light binning compute path.
// Light data and tile buffers are accessed via buffer-device-address (BDA)
// from the push constant block.

#include "include/DevicePointer.slangh"

struct CullPush
{
    DevicePtr<float4> lightData;
    DevicePtr<uint>   tileHeadersFlat; // [tile*2+0]=offset, [tile*2+1]=count
    DevicePtr<uint>   tileLightIndices;
    float4x4 viewProj;
    float4   params0; // x=nearClip, y=pixelScaleY, z=screenW, w=screenH
    uint4    params1; // x=tilePx, y=tilesX, z=tilesY, w=lightCount
    uint4    params2; // x=maxLightsPerTile
};
[[vk::push_constant]] CullPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void initTiles(uint3 tid : SV_DispatchThreadID)
{
    uint tilesX = pc.params1.y;
    uint tilesY = pc.params1.z;
    uint tileCount = tilesX * tilesY;
    uint tile = tid.x;
    if (tile >= tileCount)
    {
        return;
    }

    uint base = tile * 2u;
    pc.tileHeadersFlat.Store(base + 0u, tile * pc.params2.x);
    pc.tileHeadersFlat.Store(base + 1u, 0u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void binLights(uint3 tid : SV_DispatchThreadID)
{
    uint lightIndex = tid.x;
    uint lightCount = pc.params1.w;
    if (lightIndex >= lightCount)
    {
        return;
    }

    uint base = lightIndex * 5u;
    float4 p0 = pc.lightData.Load(base + 0u);
    float3 worldPos = p0.xyz;
    float radius = max(p0.w, 1e-4);

    float4 clip = mul(pc.viewProj, float4(worldPos, 1.0));
    float depth = abs(clip.w);
    if (depth < pc.params0.x || abs(clip.w) <= 1e-6)
    {
        return;
    }

    float3 ndc = clip.xyz / clip.w;
    float screenW = pc.params0.z;
    float screenH = pc.params0.w;
    float screenX = (ndc.x * 0.5 + 0.5) * screenW;
    float screenY = (ndc.y * 0.5 + 0.5) * screenH;
    float effectiveDepth = max(sqrt(max(depth * depth - radius * radius, 0.0)), pc.params0.x);
    float radiusPx = (radius * pc.params0.y) / effectiveDepth;
    if (radiusPx <= 0.5)
    {
        return;
    }

    float minX = screenX - radiusPx;
    float maxX = screenX + radiusPx;
    float minY = screenY - radiusPx;
    float maxY = screenY + radiusPx;
    if (maxX < 0.0 || maxY < 0.0 || minX >= screenW || minY >= screenH)
    {
        return;
    }

    uint tilePx = max(pc.params1.x, 1u);
    uint tilesX = pc.params1.y;
    uint tilesY = pc.params1.z;
    uint maxLightsPerTile = max(pc.params2.x, 1u);

    uint minTx = min((uint)max(floor(minX / tilePx), 0.0), tilesX - 1u);
    uint maxTx = min((uint)max(floor(maxX / tilePx), 0.0), tilesX - 1u);
    uint minTy = min((uint)max(floor(minY / tilePx), 0.0), tilesY - 1u);
    uint maxTy = min((uint)max(floor(maxY / tilePx), 0.0), tilesY - 1u);

    for (uint ty = minTy; ty <= maxTy; ++ty)
    {
        for (uint tx = minTx; tx <= maxTx; ++tx)
        {
            uint tile = ty * tilesX + tx;
            uint headerBase = tile * 2u;
            uint slot = 0u;
            uint* headersPtr = pc.tileHeadersFlat.Ptr();
            InterlockedAdd(headersPtr[headerBase + 1u], 1u, slot);
            if (slot < maxLightsPerTile)
            {
                uint writeIdx = headersPtr[headerBase + 0u] + slot;
                pc.tileLightIndices.Store(writeIdx, lightIndex);
            }
        }
    }
}
