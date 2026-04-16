// Batched UI shape shader.
// One indirect-instanced draw emits rectangles/circles/lines from a
// device-addressable command buffer.

static const uint kShapeRect = 0;
static const uint kShapeCircle = 1;
static const uint kShapeLine = 2;

struct DrawCommandData
{
    float4 data0;
    float4 data1;
    float4 color;
    uint type;
    int layer;
    uint _pad0;
    uint _pad1;
};

struct ShapesPush
{
    float4 screenSize; // .xy = viewport dimensions
    uint64_t commandDataAddr;
    uint _pad0;
    uint _pad1;
};

[[vk::push_constant]] ShapesPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    float2 local : TEXCOORD1;
    float2 localScale : TEXCOORD2;
    float cornerRadius : TEXCOORD3;
    nointerpolation uint type : TEXCOORD4;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    DrawCommandData* commands = (DrawCommandData*)pc.commandDataAddr;
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

    if (cmd.type == kShapeRect)
    {
        const float2 minPt = cmd.data0.xy;
        const float2 size = cmd.data0.zw;
        sp = minPt + uv * size;
    }
    else if (cmd.type == kShapeCircle)
    {
        const float2 center = cmd.data0.xy;
        const float radius = cmd.data0.z;
        sp = center + (uv * 2.0 - 1.0) * radius;
    }
    else // line
    {
        const float2 a = cmd.data0.xy;
        const float2 b = cmd.data0.zw;
        const float2 ab = b - a;
        const float len = max(length(ab), 0.0001);
        const float2 dir = ab / len;
        const float2 n = float2(-dir.y, dir.x);
        const float thickness = max(cmd.data1.x, 1.0);

        const float2 local = float2(uv.x * len, (uv.y - 0.5) * thickness);
        sp = a + dir * local.x + n * local.y;
    }

    const float ndcX = (sp.x / pc.screenSize.x) * 2.0f - 1.0f;
    const float ndcY = (sp.y / pc.screenSize.y) * 2.0f - 1.0f;

    VSOutput output;
    output.position = float4(ndcX, ndcY, 0.0, 1.0);
    output.color = cmd.color;
    output.local = uv * 2.0 - 1.0;
    output.localScale = cmd.data0.zw;
    output.cornerRadius = cmd.data1.x;
    output.type = cmd.type;
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

    if (input.type == kShapeRect && input.cornerRadius > 0.5)
    {
        const float2 halfSize = max(input.localScale * 0.5, float2(0.001, 0.001));
        const float radius = min(input.cornerRadius, min(halfSize.x, halfSize.y));
        const float2 p = abs(input.local * halfSize);
        const float2 q = p - (halfSize - radius);
        const float signedDist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        const float aa = max(fwidth(signedDist), 0.001);
        const float alpha = saturate(0.5 - signedDist / aa);
        if (alpha <= 0.001)
            discard;
        return float4(input.color.rgb, input.color.a * alpha);
    }

    return input.color;
}
