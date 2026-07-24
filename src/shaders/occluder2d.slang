// 2D shadow occluder mask. Renders each solid tile cell as a white quad into an R8 target; the
// $Light2DComposite pass ray-marches this mask to shadow the light pools. One instanced draw over the
// occluder buffer (xy = world centre, z = half-cell), transformed to clip by the scene's viewProj so
// the mask lands in the same screen space the light pass samples.
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

struct OccluderPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<float4> occluders; // xy = world centre, z = half-cell size, w unused
    uint   count;
    uint   pad;
};

[[vk::push_constant]] OccluderPush pc;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(-1, 1),
    };
    const float4 occ = pc.occluders.Ptr()[instanceId];
    const float2 world = occ.xy + corners[vertexId % 6u] * occ.z;
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    VSOutput o;
    o.position = mul(frame->viewProj, float4(world, 0.0, 1.0));
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    return float4(1.0, 0.0, 0.0, 0.0); // R8 stores .r = full occlusion
}
