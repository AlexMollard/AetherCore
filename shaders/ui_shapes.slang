// Batched UI shape shader.
// One indirect-instanced draw emits rectangles/circles/lines/textured-rects/SDF-glyphs
// from a device-addressable command buffer.

#include "include/UIStructs.slangh"
#include "text_sdf.slangh"

static const uint kShapeRect        = 0;
static const uint kShapeCircle      = 1;
static const uint kShapeLine        = 2;
static const uint kShapeTexturedRect = 3;
static const uint kShapeSdfGlyph    = 4;

// Bindless resources (set 0).
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct ShapesPush
{
    float4   screenSize; // .xy = viewport dimensions
    DevicePtr<DrawCommandData> commandData;
    uint     _pad0;
    uint     _pad1;
};

[[vk::push_constant]] ShapesPush pc;

struct VSOutput
{
    float4 position      : SV_Position;
    float4 color         : TEXCOORD0;
    float2 local         : TEXCOORD1; // [-1,1] for SDF corner/circle tests
    float2 localScale    : TEXCOORD2; // rect pixel size (for corner radius)
    float  cornerRadius  : TEXCOORD3;
    nointerpolation uint type        : TEXCOORD4;
    float2 texUV         : TEXCOORD5; // UV for TexturedRect
    nointerpolation uint textureSlot : TEXCOORD6;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    DrawCommandData* commands = pc.commandData.Ptr();
    const DrawCommandData cmd = commands[instanceId];

    const float2 uvRect[6] = {
        float2(0.0, 1.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(0.0, 0.0),
    };

    float2 sp = float2(0.0, 0.0);
    const float2 uv = uvRect[vertexId % 6u];
    float2 outTexUV = float2(0.0, 0.0);

    if (cmd.type == kShapeRect || cmd.type == kShapeTexturedRect || cmd.type == kShapeSdfGlyph)
    {
        const float2 minPt = cmd.data0.xy;
        const float2 size  = cmd.data0.zw;
        sp = minPt + uv * size;

        if (cmd.type == kShapeTexturedRect || cmd.type == kShapeSdfGlyph)
        {
            // data1 = u0, v0, u1, v1
            outTexUV = float2(
                lerp(cmd.data1.x, cmd.data1.z, uv.x),
                lerp(cmd.data1.y, cmd.data1.w, uv.y)
            );
        }
    }
    else if (cmd.type == kShapeCircle)
    {
        const float2 center = cmd.data0.xy;
        const float radius  = cmd.data0.z;
        sp = center + (uv * 2.0 - 1.0) * radius;
    }
    else // kShapeLine
    {
        const float2 a  = cmd.data0.xy;
        const float2 b  = cmd.data0.zw;
        const float2 ab = b - a;
        const float  len = max(length(ab), 0.0001);
        const float2 dir = ab / len;
        const float2 n   = float2(-dir.y, dir.x);
        const float thickness = max(cmd.data1.x, 1.0);

        const float2 local = float2(uv.x * len, (uv.y - 0.5) * thickness);
        sp = a + dir * local.x + n * local.y;
    }

    const float ndcX = (sp.x / pc.screenSize.x) * 2.0f - 1.0f;
    const float ndcY = (sp.y / pc.screenSize.y) * 2.0f - 1.0f;

    VSOutput output;
    output.position    = float4(ndcX, ndcY, 0.0, 1.0);
    output.color       = cmd.color;
    output.local       = uv * 2.0 - 1.0;
    output.localScale  = cmd.data0.zw;
    output.cornerRadius = (cmd.type == kShapeTexturedRect || cmd.type == kShapeSdfGlyph) ? 0.0 : cmd.data1.x;
    output.type        = cmd.type;
    output.texUV       = outTexUV;
    output.textureSlot = cmd.textureSlot;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    if (input.type == kShapeCircle)
    {
        if (dot(input.local, input.local) > 1.0)
            discard;
        return input.color;
    }

    if (input.type == kShapeSdfGlyph)
    {
        const float alpha = SdfGlyphAlpha(g_textures[input.textureSlot], g_linearSampler, input.texUV);
        if (alpha <= 0.001)
            discard;
        return float4(input.color.rgb, input.color.a * alpha);
    }

    if (input.type == kShapeTexturedRect)
    {
        const float4 texColor = g_textures[input.textureSlot].Sample(g_linearSampler, input.texUV);
        return texColor * input.color;
    }

    if (input.type == kShapeRect && input.cornerRadius > 0.5)
    {
        const float2 halfSize = max(input.localScale * 0.5, float2(0.001, 0.001));
        const float radius    = min(input.cornerRadius, min(halfSize.x, halfSize.y));
        const float2 p        = abs(input.local * halfSize);
        const float2 q        = p - (halfSize - radius);
        const float signedDist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        const float aa = max(fwidth(signedDist), 0.001);
        const float alpha = saturate(0.5 - signedDist / aa);
        if (alpha <= 0.001)
            discard;
        return float4(input.color.rgb, input.color.a * alpha);
    }

    return input.color;
}
