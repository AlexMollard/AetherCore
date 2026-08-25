// GPU tiled-light binning, one workgroup per tile (cooperative).
//
// Works for ANY view (main camera, camera preview, RTT cameras): the push
// constants carry the view matrix + projection scales, so culling happens in
// that view's eye space with exact per-tile frustum planes - no screen-space
// bound approximations and no "light crosses the near plane -> every tile"
// fallback. Spot lights get a cone-vs-plane refinement after the bounding
// sphere passes, so a narrow cone no longer bins into every tile its sphere
// touches.
//
// Dispatch: (tilesX, tilesY, 1) groups x 64 threads. Threads stride the light
// list cooperatively and append hits to a groupshared list, then write the
// tile's index range + header. O(tiles x lights / 64) instead of the previous
// one-thread-per-tile O(tiles x lights) scalar loop.
//
// Light data and tile buffers are accessed via buffer-device-address (BDA)
// from the push constant block. Light layout: 5x float4 per light
// (positionRadius, colorIntensity, directionType, params(innerCos,outerCos),
// shadowIndex).

#include "include/DevicePointer.slangh"

struct CullPush
{
    DevicePtr<float4> lightData;
    DevicePtr<uint>   tileHeadersFlat; // [tile*2+0]=offset, [tile*2+1]=count
    DevicePtr<uint>   tileLightIndices;
    float4x4 view;    // world -> view (NOT viewProj: culling runs in eye space)
    float3   params0; // x=nearClip, y=1/proj[0][0], z=1/proj[1][1] (signed)
    uint4    params1; // x=tilePx, y=tilesX, z=tilesY, w=lightCount
    uint3    params2; // x=maxLightsPerTile, y=screenW(px), z=screenH(px)
};
[[vk::push_constant]] CullPush pc;

#define MAX_LIGHTS_PER_TILE 128u

groupshared uint s_count;
groupshared uint s_list[MAX_LIGHTS_PER_TILE];

// Max over the cone (apex A, unit axis D, axial length h, tan(halfAngle)) of the
// signed distance to a plane through the origin with unit normal n. The extreme
// point is either the apex or a point on the base rim; the rim maximum is
// dist(rim centre) + rimRadius * |n perpendicular to D|. Exact for cone-vs-plane,
// conservative when combined per-frustum-plane.
float ConeMaxSignedDistance(float3 n, float3 A, float3 D, float h, float tanHalf)
{
    const float apexDist = dot(n, A);
    const float3 nPerp = n - D * dot(n, D);
    const float rimDist = dot(n, A + D * h) + h * tanHalf * length(nPerp);
    return max(apexDist, rimDist);
}

// Named `main`: slangc renames a module's single entry point to "main" in the
// emitted SPIR-V regardless of the function name, so name it that explicitly
// to keep the C++ pipeline's shaderEntry in sync.
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupTid : SV_GroupThreadID)
{
    const uint tilesX = pc.params1.y;
    const uint tilesY = pc.params1.z;
    const uint tileX = groupId.x;
    const uint tileY = groupId.y;
    if (tileX >= tilesX || tileY >= tilesY)
    {
        return;
    }

    const uint lane = groupTid.x;
    if (lane == 0u)
    {
        s_count = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint  tilePx = max(pc.params1.x, 1u);
    const float screenW = max((float)pc.params2.y, 1.0);
    const float screenH = max((float)pc.params2.z, 1.0);
    const float nearClip = pc.params0.x;
    const float invP00 = pc.params0.y; // = tan(halfFovX), maps ndc.x -> view-space slope
    const float invP11 = pc.params0.z; // signed: carries the Vulkan Y flip

    // Tile pixel bounds -> NDC -> view-space slopes (x/-z and y/-z at the tile
    // edges). The camera looks down -Z; a point at slope s lies at x = s * (-z).
    const float px0 = (float)(tileX * tilePx);
    const float px1 = min(px0 + (float)tilePx, screenW);
    const float py0 = (float)(tileY * tilePx);
    const float py1 = min(py0 + (float)tilePx, screenH);

    const float sx0 = (px0 / screenW * 2.0 - 1.0) * invP00;
    const float sx1 = (px1 / screenW * 2.0 - 1.0) * invP00;
    const float syA = (py0 / screenH * 2.0 - 1.0) * invP11;
    const float syB = (py1 / screenH * 2.0 - 1.0) * invP11;
    const float syMin = min(syA, syB); // invP11 is signed; order the slopes explicitly
    const float syMax = max(syA, syB);

    // Four tile-frustum side planes through the eye (normalized, inward-positive).
    const float3 planeL = float3(1.0, 0.0, sx0) * rsqrt(1.0 + sx0 * sx0);
    const float3 planeR = float3(-1.0, 0.0, -sx1) * rsqrt(1.0 + sx1 * sx1);
    const float3 planeB = float3(0.0, 1.0, syMin) * rsqrt(1.0 + syMin * syMin);
    const float3 planeT = float3(0.0, -1.0, -syMax) * rsqrt(1.0 + syMax * syMax);

    const uint lightCount = pc.params1.w;
    const uint maxLightsPerTile = min(max(pc.params2.x, 1u), MAX_LIGHTS_PER_TILE);

    for (uint lightIndex = lane; lightIndex < lightCount; lightIndex += 64u)
    {
        const uint base = lightIndex * 5u;
        const float4 p0 = pc.lightData.Load(base + 0u); // xyz=world pos, w=radius/range
        const float radius = max(p0.w, 1e-4);
        const float3 posV = mul(pc.view, float4(p0.xyz, 1.0)).xyz;

        // Fully behind the near plane (camera looks down -Z).
        const float depth = -posV.z;
        if (depth + radius < nearClip)
        {
            continue;
        }

        // Bounding sphere vs the tile frustum side planes.
        if (dot(planeL, posV) < -radius || dot(planeR, posV) < -radius ||
            dot(planeB, posV) < -radius || dot(planeT, posV) < -radius)
        {
            continue;
        }

        // Spot cone refinement: the sphere around the whole range passes easily,
        // but the cone itself often misses the tile entirely.
        const float4 p2 = pc.lightData.Load(base + 2u); // xyz=direction, w=type (1=spot)
        if (p2.w > 0.5)
        {
            const float4 p3 = pc.lightData.Load(base + 3u); // x=innerCos, y=outerCos
            const float cosOuter = p3.y;
            if (cosOuter > 0.05) // very wide cones degenerate to the sphere test
            {
                const float3 dirV = normalize(mul(pc.view, float4(p2.xyz, 0.0)).xyz);
                const float tanOuter = sqrt(max(1.0 - cosOuter * cosOuter, 0.0)) / cosOuter;
                if (ConeMaxSignedDistance(planeL, posV, dirV, radius, tanOuter) < 0.0 ||
                    ConeMaxSignedDistance(planeR, posV, dirV, radius, tanOuter) < 0.0 ||
                    ConeMaxSignedDistance(planeB, posV, dirV, radius, tanOuter) < 0.0 ||
                    ConeMaxSignedDistance(planeT, posV, dirV, radius, tanOuter) < 0.0)
                {
                    continue;
                }
            }
        }

        uint slot;
        InterlockedAdd(s_count, 1u, slot);
        if (slot < maxLightsPerTile)
        {
            s_list[slot] = lightIndex;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    const uint tile = tileY * tilesX + tileX;
    const uint tileOffset = tile * maxLightsPerTile;
    const uint outCount = min(s_count, maxLightsPerTile);
    for (uint i = lane; i < outCount; i += 64u)
    {
        pc.tileLightIndices.Store(tileOffset + i, s_list[i]);
    }
    if (lane == 0u)
    {
        pc.tileHeadersFlat.Store(tile * 2u + 0u, tileOffset);
        pc.tileHeadersFlat.Store(tile * 2u + 1u, outCount);
    }
}
