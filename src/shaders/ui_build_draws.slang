// UI compute preprocess.
// GPU-side layer sort of DrawCommandData and VkDrawIndirectCommand write.
//
// For N <= kMaxLocalSort: single-workgroup shared-memory bitonic sort.
// For N >  kMaxLocalSort: multi-pass global swap using odd-even merge steps.

#include "include/UIStructs.slangh"

static const uint kMaxLocalSort = 256u;
static const uint kThreads      = 256u;

struct BuildPush
{
	DevicePtr<DrawCommandData>   commandData;
	DevicePtr<DrawIndirectCommand> indirectCmd;
	uint N;            // padded element count (next power of two)
	uint stage;        // bitonic stage (2,4,8,...,N) for global path, or 0 for indirect-write
	uint step;         // compare distance (global path only)
	uint actualCount;  // original count for DrawIndirectCommand
};

[[vk::push_constant]] BuildPush pc;

// -- Shared-memory sort (N <= kMaxLocalSort) ----------------------------------
// Single dispatch of kThreads threads.  Loads elements into groupshared memory,
// runs a full in-workgroup bitonic sort, then writes back.

groupshared DrawCommandData g_LocalData[kMaxLocalSort];

[shader("compute")]
[numthreads(kThreads, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
	if (pc.stage == 0u)
	{
		writeIndirectPass(dtid);
		return;
	}

	if (pc.N <= kMaxLocalSort)
	{
		localSortPass(tid);
	}
	else
	{
		globalSwapPass(dtid);
	}
}

	// Load elements into shared memory, run local bitonic sort, write back.
	void localSortPass(uint3 tid)
	{
		DrawCommandData* commands = pc.commandData.Ptr();

		// Coalesced load. Threads beyond pc.N must still reach every barrier
		// below but must never touch shared memory (out-of-bounds).
		bool active = tid.x < pc.N;
		if (active)
		{
			for (uint i = tid.x; i < pc.N; i += kThreads)
			{
				g_LocalData[i] = commands[i];
			}
		}

		GroupMemoryBarrierWithGroupSync();

		// In-place bitonic sort within the single workgroup.
		for (uint k = 2u; k <= pc.N; k <<= 1u)
		{
			for (uint j = k >> 1u; j > 0u; j >>= 1u)
			{
				if (active)
				{
					uint i = tid.x;
					uint ix = i ^ j;

					if (ix > i)
					{
						bool ascending = ((i & k) == 0u);
						int aLayer = g_LocalData[i].layer;
						int bLayer = g_LocalData[ix].layer;
						bool shouldSwap = ascending ? (aLayer > bLayer) : (aLayer < bLayer);
						if (shouldSwap)
						{
							DrawCommandData tmp = g_LocalData[i];
							g_LocalData[i] = g_LocalData[ix];
							g_LocalData[ix] = tmp;
						}
					}
				}

				GroupMemoryBarrierWithGroupSync();
			}
		}

		// Write sorted data back.
		GroupMemoryBarrierWithGroupSync();
		if (active)
		{
			for (uint i = tid.x; i < pc.N; i += kThreads)
			{
				commands[i] = g_LocalData[i];
			}
		}
	}

// -- Global multi-pass swap (N > kMaxLocalSort) -------------------------------
// One dispatch per (stage, step) pair.  Each thread handles one element pair;
// dispatched with N/2 threads total.

void globalSwapPass(uint3 dtid)
{
	uint i = dtid.x;
	uint halfN = pc.N >> 1u;
	if (i >= halfN)
		return;

	DrawCommandData* commands = pc.commandData.Ptr();

	uint ix = i ^ pc.step;
	bool ascending = ((i & pc.stage) == 0u);

	int aLayer = commands[i].layer;
	int bLayer = commands[ix].layer;

	bool shouldSwap = ascending ? (aLayer > bLayer) : (aLayer < bLayer);

	if (shouldSwap)
	{
		DrawCommandData tmp = commands[i];
		commands[i] = commands[ix];
		commands[ix] = tmp;
	}
}

// -- Indirect-command write pass ------------------------------------------------
// Single dispatch (1,1,1) after all sort passes complete.

void writeIndirectPass(uint3 dtid)
{
	if (dtid.x != 0u)
		return;

	DrawIndirectCommand* indirect = pc.indirectCmd.Ptr();
	indirect[0].vertexCount = 6;
	indirect[0].instanceCount = pc.actualCount;
	indirect[0].firstVertex = 0;
	indirect[0].firstInstance = 0;
}
