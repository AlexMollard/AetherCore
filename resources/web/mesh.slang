// Unlit geometry/depth foundation. Production PBR/effect parity is not claimed.
struct FrameData
{
    float4 clip0;
    float4 clip1;
    float4 clip2;
    float4 clip3;
};
struct MeshData
{
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    float4 normal0;
    float4 normal1;
    float4 normal2;
    float4 color;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] StructuredBuffer<MeshData> meshes;
struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};
[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION, float3 color : COLOR0, uint instance : SV_InstanceID)
{
    MeshData mesh = meshes[instance];
    float4 world = mesh.world0 * position.x + mesh.world1 * position.y + mesh.world2 * position.z + mesh.world3;
    VertexOutput output;
    output.position = float4(dot(frame.clip0,world),dot(frame.clip1,world),dot(frame.clip2,world),dot(frame.clip3,world));
    output.position.y = -output.position.y;
    output.color = mesh.color * float4(color, 1);
    return output;
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return input.color;
}
