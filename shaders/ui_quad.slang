// Solid-color UI quad shader.
// Vertices are generated procedurally from SV_VertexID (6 vertices per quad).

struct QuadPush
{
    float4 screenSize; // .xy = viewport dimensions
    float4 rect;       // x, y, w, h in screen pixels (top-left origin)
    float4 color;      // RGBA tint
};

[[vk::push_constant]] QuadPush pc;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    const float x0 = pc.rect.x;
    const float y0 = pc.rect.y;
    const float x1 = pc.rect.x + pc.rect.z;
    const float y1 = pc.rect.y + pc.rect.w;

    const float2 positions[6] = {
        float2(x0, y1),
        float2(x1, y1),
        float2(x1, y0),
        float2(x0, y1),
        float2(x1, y0),
        float2(x0, y0),
    };

    const float2 sp = positions[vertexId];
    const float ndcX = (sp.x / pc.screenSize.x) * 2.0f - 1.0f;
    const float ndcY = (sp.y / pc.screenSize.y) * 2.0f - 1.0f;

    VSOutput output;
    output.position = float4(ndcX, ndcY, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    return pc.color;
}
