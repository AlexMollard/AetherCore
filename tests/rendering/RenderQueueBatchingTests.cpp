#include <doctest/doctest.h>
#include "rendering/BatchRuns.hpp"

using namespace aether::render_queue_batching;

namespace
{
	// Tag pointers only need identity for the run counter; never dereferenced.
	int kPipelineA = 0;
	int kPipelineB = 0;
	int kMesh0 = 0;
	int kMesh1 = 0;
	int kMesh2 = 0;

	DrawKey Draw(const bool blended, const void* pipeline, const void* mesh)
	{
		return DrawKey{.blended = blended, .pipeline = pipeline, .mesh = mesh};
	}
}

TEST_CASE("Opaque draws of one pipeline and mesh collapse into a single run")
{
	const DrawKey draws[] = {
	        Draw(false, &kPipelineA, &kMesh0),
	        Draw(false, &kPipelineA, &kMesh0),
	        Draw(false, &kPipelineA, &kMesh0),
	};
	const BatchRunStats stats = CountBatchRuns(draws, draws + 3);
	CHECK(stats.runs == 1);
	CHECK(stats.blendedDraws == 0);
}

TEST_CASE("A new mesh or pipeline starts a new run")
{
	const DrawKey draws[] = {
	        Draw(false, &kPipelineA, &kMesh0),
	        Draw(false, &kPipelineA, &kMesh1),
	        Draw(false, &kPipelineB, &kMesh1),
	        Draw(false, &kPipelineA, &kMesh1),
	};
	const BatchRunStats stats = CountBatchRuns(draws, draws + 4);
	CHECK(stats.runs == 4);
}

TEST_CASE("Depth-sorted blended draws each need their own run")
{
	// The hub's transparent population: hundreds of small instances of a handful of
	// meshes, far-to-near. Depth order interleaves the meshes, so consecutive draws
	// rarely share (pipeline, mesh) and batching collapses to roughly one run per
	// draw - blended runs scale with instance count, not mesh count.
	const DrawKey draws[] = {
	        Draw(true, &kPipelineA, &kMesh0),
	        Draw(true, &kPipelineA, &kMesh1),
	        Draw(true, &kPipelineA, &kMesh0),
	        Draw(true, &kPipelineA, &kMesh1),
	};
	const BatchRunStats stats = CountBatchRuns(draws, draws + 4);
	CHECK(stats.runs == 4);
	CHECK(stats.blendedDraws == 4);

	// Identical (pipeline, mesh) neighbours - the same instance cluster at one depth -
	// still batch.
	const DrawKey sameDepth[] = {
	        Draw(true, &kPipelineA, &kMesh0),
	        Draw(true, &kPipelineA, &kMesh0),
	};
	CHECK(CountBatchRuns(sameDepth, sameDepth + 2).runs == 1);
}

TEST_CASE("Blended-to-opaque transitions split runs even within one mesh")
{
	const DrawKey draws[] = {
	        Draw(false, &kPipelineA, &kMesh2),
	        Draw(true, &kPipelineA, &kMesh2),
	        Draw(false, &kPipelineA, &kMesh2),
	};
	CHECK(CountBatchRuns(draws, draws + 3).runs == 3);
}

TEST_CASE("Empty draw lists have no runs")
{
	const DrawKey draws[] = {Draw(false, &kPipelineA, &kMesh0)};
	const BatchRunStats stats = CountBatchRuns(draws, draws);
	CHECK(stats.runs == 0);
	CHECK(stats.blendedDraws == 0);
	CHECK(CountUniqueMeshes(draws, draws) == 0);
}

TEST_CASE("Unique mesh count collapses instances sharing a mesh")
{
	const DrawKey draws[] = {
	        Draw(false, &kPipelineA, &kMesh0),
	        Draw(false, &kPipelineA, &kMesh0),
	        Draw(false, &kPipelineA, &kMesh1),
	        Draw(true, &kPipelineA, &kMesh0),
	        Draw(true, &kPipelineA, &kMesh2),
	};
	CHECK(CountUniqueMeshes(draws, draws + 5) == 3);
}

TEST_CASE("Batch capacity grows to at least the requirement and amortises")
{
	// At or above the requirement the capacity is kept as-is.
	CHECK(GrownBatchCapacity(1024, 512) == 1024);
	CHECK(GrownBatchCapacity(1024, 1024) == 1024);
	// Below it, doubling from the current capacity overtakes the requirement.
	CHECK(GrownBatchCapacity(1024, 1025) == 2048);
	CHECK(GrownBatchCapacity(1024, 4000) == 4096);
	// A zero-capacity slot (never grown) starts from the config-scale default.
	CHECK(GrownBatchCapacity(0, 1) == 1024);
	CHECK(GrownBatchCapacity(0, 2000) == 2048);
}
