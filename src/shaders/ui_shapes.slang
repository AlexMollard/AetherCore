// Batched UI shape shader.
// One indirect-instanced draw emits rectangles/circles/lines/textured-rects/SDF-glyphs
// from a device-addressable command buffer.

#include "include/UIStructs.slangh"
#include "include/UiVertexOut.slangh"
#include "text_curves.slangh"

static const uint kShapeRect        = 0;
static const uint kShapeCircle      = 1;
static const uint kShapeLine        = 2;
static const uint kShapeTexturedRect = 3;
static const uint kShapeGlyph       = 4;

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

    if (cmd.type == kShapeRect || cmd.type == kShapeTexturedRect || cmd.type == kShapeGlyph)
    {
        const float2 minPt = cmd.data0.xy;
        const float2 size  = cmd.data0.zw;
        sp = minPt + uv * size;

        if (cmd.type == kShapeTexturedRect || cmd.type == kShapeGlyph)
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
    output.cornerRadius = (cmd.type == kShapeTexturedRect || cmd.type == kShapeGlyph) ? 0.0 : cmd.data1.x;
    output.type        = cmd.type;
    output.texUV       = outTexUV;
    output.glyphBands  = cmd.data1;
    output.textureSlot = cmd.textureSlot;
    output.flags       = cmd.flags;
    output.clipRect    = cmd.clipRect;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    if (input.type == kShapeCircle)
    {
        if (dot(input.local, input.local) > 1.0)
            discard;
        return input.color;
    }

    if (input.type == kShapeGlyph)
    {
        // The quad IS the glyph's outline box, so the interpolated local coordinate is a
        // position in the glyph's own [0,1] curve space - after flipping Y, because UI space
        // runs downward and font outlines run upward.
        const float2 glyphLocal = float2(input.local.x * 0.5 + 0.5, 0.5 - input.local.y * 0.5);
        // One pixel measured in that same space, which is what makes the coverage filter
        // exactly one pixel wide at any size.
        const float2 pixelSize = 1.0 / max(input.localScale, float2(1e-4, 1e-4));

        uint texWidth, texHeight;
        g_textures[input.textureSlot].GetDimensions(texWidth, texHeight);

        const float alpha = GlyphCoverage(
            g_textures[input.textureSlot], texWidth, glyphLocal, pixelSize,
            uint(input.glyphBands.x), uint(input.glyphBands.y), uint(input.glyphBands.z));
        if (alpha <= 0.002)
            discard;
        return float4(input.color.rgb, input.color.a * alpha);
    }

    if (input.type == kShapeTexturedRect)
    {
        float2 uv = input.texUV;
        if ((input.flags & 1u) != 0u)
        {
            // Pixel art: snap to texel centres so icons stay crisp under the
            // shared linear sampler.
            uint width, height;
            g_textures[input.textureSlot].GetDimensions(width, height);
            const float2 texSize = float2(max(width, 1u), max(height, 1u));
            uv = (floor(uv * texSize) + 0.5) / texSize;
        }
        const float4 texColor = g_textures[input.textureSlot].Sample(g_linearSampler, uv);
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
