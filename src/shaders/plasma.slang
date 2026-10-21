// Animated spherical plasma ball shader.
//
// Reads per-entity EffectParams (tint / speed / scale / intensity) by name from
// the EffectParamBuffer, indexed by the per-entity effectParamIndex varying.
//
// Uses FrameConstantsData.elapsedTime for time-driven animation.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// This effect shader consumes the per-entity EffectParams slot, so opt into the
// effectParamIndex vertex->fragment varying (location 8) before pulling in the
// shared mesh vertex output / vertexMain.
#define AETHER_VS_EFFECT_PARAMS 1

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/EffectParams.slangh"
#include "include/MeshVertex.slangh"
#include "include/DefaultVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

#include "include/ProceduralMaterialMath.slangh"

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* fc = pc.frame.Ptr();
    float elapsed = fc->elapsedTime;

    EffectParams fx;
    fx.tint = float4(1.0, 1.0, 1.0, 1.0);
    fx.speed = 1.0;
    fx.scale = 1.0;
    fx.intensity = 1.0;
    // Guard on the BDA too: passes that do not wire the effect buffer (e.g.
    // render-to-texture camera views) ship effectParamBufferAddr == 0, and
    // dereferencing it would be a null read. Fall back to default params there.
    if (input.effectParamIndex != 0xFFFFFFFFu && fc->effectParamBufferAddr != 0)
    {
        EffectParams* params = (EffectParams*)(fc->effectParamBufferAddr);
        fx = params[input.effectParamIndex];
    }

    return SharedPlasmaShade(input.worldNormal, input.worldTangent, input.tangentSign, input.uv2,
        input.worldPos, fc->cameraWorldPos.xyz, elapsed, fx);
}
