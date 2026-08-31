// Proof that a PROJECT shader can be a mesh material shader, which is what a material graph
// would have to generate. Include order mirrors gltf_mesh.slang: DefaultVertex.slangh brings
// the whole vertex stage, so a generated shader only ever has to write a fragment.
#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/GpuMaterial.slangh"
#include "include/MeshVertex.slangh"
#include "include/DefaultVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    return float4(0.05, 0.95, 0.25, 1.0);
}
