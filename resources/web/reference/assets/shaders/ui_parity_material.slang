// Per-element UI material; the reference uses it on a rectangular image.
#include "include/UiVertexOut.slangh"
struct ShapesPush
{
    float4 screenSize;
    uint64_t commandData;
    uint pad0;
    uint pad1;
    float4 params;
    float4 color0;
    float4 color1;
};
[[vk::push_constant]] ShapesPush pc;
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);
    return pc.color0 * input.color;
}
