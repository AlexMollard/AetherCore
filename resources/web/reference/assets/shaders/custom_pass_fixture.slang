// Parity fixture for the portable CustomPass contract (include/CustomPass.slangh). One shader, three
// stages: the script submits hard-edged world-space rectangles, the shader fills them with color0 and
// draws a border on the MIN-x and MIN-y edges only (an L shape, so mirrored or flipped output is
// visible) in color1. Fully deterministic: no elapsed time, no noise, alpha is 1 inside and the
// fragment is discarded outside.
//
// Data layout, two float4 per rectangle i:
//   CustomPassData(2i)     = (min.x, min.y, max.x, max.y)  world rectangle, min inclusive / max exclusive
//   CustomPassData(2i + 1) = (border width in world units, unused, unused, unused)
// params = world AABB (min.xy, max.xy) of every rectangle; the quad is bounded to it. A collapsed AABB
// draws nothing.
#include "include/CustomPass.slangh"

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1),
    };
    const float2 c = corners[vertexId % 6u];

    const float4 params = CustomPassParams();
    VSOutput o;
    o.uv = c;
    if (params.z <= params.x || params.w <= params.y)
    {
        o.position = float4(0.0, 0.0, 2.0, 1.0); // behind the near plane -> clipped
        return o;
    }
    const float2 world = lerp(params.xy, params.zw, c);
    o.position = mul(CustomPassViewProj(), float4(world, 0.0, 1.0));
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float2 ndc = input.position.xy / max(CustomPassViewportSize(), float2(1.0, 1.0)) * 2.0 - 1.0;
    const float4 wp = mul(CustomPassInvViewProj(), float4(ndc, 0.0, 1.0));
    const float2 world = wp.xy / wp.w;

    const uint rectCount = CustomPassDataCount() / 2u;
    for (uint i = 0u; i < rectCount; ++i)
    {
        const float4 rect = CustomPassData(2u * i);
        const float border = CustomPassData(2u * i + 1u).x;
        if (world.x >= rect.x && world.y >= rect.y && world.x < rect.z && world.y < rect.w)
        {
            const bool onBorder = world.x < rect.x + border || world.y < rect.y + border;
            return float4(onBorder ? CustomPassColor1().rgb : CustomPassColor0().rgb, 1.0);
        }
    }
    discard;
    return float4(0.0, 0.0, 0.0, 0.0);
}
