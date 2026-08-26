// Blends the screen-space reflection buffer over the scene.
//
// Separate from the march because that pass reads the HDR colour to fetch what a
// ray landed on, and a pass cannot sample the target it is writing. The march
// writes its own buffer; this adds it back.
//
// The SSR buffer is premultiplied - rgb already carries the confidence weight and
// a is that weight - so this is a straight additive blend and the pipeline
// supplies the add. Where a ray failed the weight is zero and nothing is
// contributed, which is what leaves the analytic sky probe showing through.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FullScreenTriangle.slangh"

struct SsrCompositePush
{
	uint ssrSlot;
	uint _pad0;
	uint _pad1;
	uint _pad2;
};
[[vk::push_constant]] SsrCompositePush pc;

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
	const float4 ssr = g_textures[pc.ssrSlot].Load(int3(int2(svPos.xy), 0));
	return float4(ssr.rgb, 1.0f);
}
