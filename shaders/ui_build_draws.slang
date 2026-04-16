// UI compute preprocess.
// Performs a simple in-place layer sort and writes VkDrawIndirectCommand.

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

struct DrawIndirectCommand
{
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

struct BuildPush
{
    uint64_t commandDataAddr;
    uint64_t indirectCmdAddr;
    uint commandCount;
    uint _pad0;
};

[[vk::push_constant]] BuildPush pc;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x != 0)
        return;

    DrawCommandData* commands = (DrawCommandData*)pc.commandDataAddr;

    // UI command counts are typically modest; insertion sort keeps code simple
    // and deterministic while running entirely on the GPU.
    for (uint i = 1; i < pc.commandCount; ++i)
    {
        DrawCommandData key = commands[i];
        int j = int(i) - 1;
        while (j >= 0 && commands[j].layer > key.layer)
        {
            commands[j + 1] = commands[j];
            --j;
        }
        commands[j + 1] = key;
    }

    DrawIndirectCommand* indirect = (DrawIndirectCommand*)pc.indirectCmdAddr;
    indirect[0].vertexCount = 6;
    indirect[0].instanceCount = pc.commandCount;
    indirect[0].firstVertex = 0;
    indirect[0].firstInstance = 0;
}
