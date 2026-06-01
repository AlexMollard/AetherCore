// depth-only shadow caster for directional light.
// Vertex data fetched via buffer device address (DrawInstanceData.vertexBufferAddr).

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/MeshVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

#include "include/DepthVertex.slangh"

[shader("vertex")]
DepthVSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    float3 worldPos;
    float4 clipPos = DepthVertexTransform(vertexId, startInstance, worldPos);

    DepthVSOutput o;
    o.svPosition = clipPos;
    return o;
}

[shader("fragment")]
void fragmentMain()
{
}
