#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace aether::render_queue_batching
{
	// GPU-free batching maths shared by RenderQueue's prepare pass and its unit tests.
	// A "batch" is one run of consecutive draws sharing (pipeline, mesh) in the sorted
	// draw list - the thing the per-queue BatchDesc buffer and the GPU cull are sized for.
	struct DrawKey
	{
		bool blended = false;
		const void* pipeline = nullptr;
		const void* mesh = nullptr;
	};

	struct BatchRunStats
	{
		std::uint32_t runs = 0;
		std::uint32_t blendedDraws = 0;
	};

	// Counts the batch runs a (pipeline, mesh)-grouping pass would emit for a draw list
	// already in its final order: opaque draws keep their pipeline-then-mesh grouping,
	// blended draws are depth-sorted first, so every blended draw of a different depth
	// is its own run. That asymmetry is why a scene full of small transparent instances
	// (wumpa, leaves) needs far more batches than its unique-mesh count suggests.
	// O(n); cheap enough for every prepare.
	[[nodiscard]] inline BatchRunStats CountBatchRuns(const DrawKey* first, const DrawKey* last)
	{
		BatchRunStats stats{};
		if (first == last)
		{
			return stats;
		}
		const DrawKey* previous = nullptr;
		for (const DrawKey* it = first; it != last; ++it)
		{
			if (previous == nullptr || it->blended != previous->blended || it->pipeline != previous->pipeline || it->mesh != previous->mesh)
			{
				++stats.runs;
			}
			if (it->blended)
			{
				++stats.blendedDraws;
			}
			previous = it;
		}
		return stats;
	}

	// Distinct non-null mesh pointers in the list. O(n log n) and allocating - for the
	// one-time overflow diagnostic only, never the per-frame path.
	[[nodiscard]] inline std::uint32_t CountUniqueMeshes(const DrawKey* first, const DrawKey* last)
	{
		std::vector<const void*> meshes;
		meshes.reserve(static_cast<std::size_t>(last - first));
		for (const DrawKey* it = first; it != last; ++it)
		{
			if (it->mesh != nullptr)
			{
				meshes.push_back(it->mesh);
			}
		}
		std::sort(meshes.begin(), meshes.end());
		return static_cast<std::uint32_t>(std::unique(meshes.begin(), meshes.end()) - meshes.begin());
	}

	// Growth policy for the per-slot BatchDesc buffer: at least the required count,
	// otherwise double - amortising repeated regrowth when a scene streams in gradually.
	[[nodiscard]] inline std::uint32_t GrownBatchCapacity(const std::uint32_t currentCapacity, const std::uint32_t required)
	{
		std::uint32_t grown = currentCapacity;
		while (grown < required)
		{
			grown = grown == 0u ? 1024u : grown * 2u;
		}
		return grown;
	}
} // namespace aether::render_queue_batching
