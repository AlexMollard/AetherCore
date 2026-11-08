#include <doctest/doctest.h>
#include "rendering/AnimJobDedupe.hpp"
#include "rendering/RenderQueue.hpp"

#include <vector>

using namespace aether;
using namespace aether::render_queue_anim;

namespace
{
	const int kDbA = 1;
	const int kDbB = 2;

	SampleKey Key(const void* db, std::uint32_t clip, float time)
	{
		return SampleKey{.db = db, .dbGeneration = 1, .clip = clip, .time = time};
	}

	// What RenderQueue does per draw: reuse the job an equal key already owns, else add one.
	std::uint32_t FindOrAdd(DedupeTable<SampleKey>& table, const SampleKey& key)
	{
		const std::uint32_t found = table.Find(key);
		return found != DedupeTable<SampleKey>::kNone ? found : table.Add(key);
	}
} // namespace

TEST_CASE("Draws sharing one animator state share one sample job")
{
	// A chicken: seven child meshes, each drawn once per queue, all carrying the same state.
	DedupeTable<SampleKey> table;
	for (int draw = 0; draw < 7; ++draw)
	{
		CHECK(FindOrAdd(table, Key(&kDbA, 3, 1.25f)) == 0u);
	}
	CHECK(table.Size() == 1u);
}

TEST_CASE("Distinct animator states get distinct sample jobs")
{
	DedupeTable<SampleKey> table;
	const std::uint32_t base = FindOrAdd(table, Key(&kDbA, 3, 1.25f));
	const std::uint32_t otherTime = FindOrAdd(table, Key(&kDbA, 3, 1.5f));
	const std::uint32_t otherClip = FindOrAdd(table, Key(&kDbA, 4, 1.25f));
	const std::uint32_t otherDb = FindOrAdd(table, Key(&kDbB, 3, 1.25f));
	SampleKey fading = Key(&kDbA, 3, 1.25f);
	fading.fadeClip = 1;
	fading.fadeTime = 0.5f;
	fading.fadeWeight = 0.3f;
	const std::uint32_t otherFade = FindOrAdd(table, fading);
	CHECK(table.Size() == 5u);
	CHECK(std::vector{base, otherTime, otherClip, otherDb, otherFade} == std::vector{0u, 1u, 2u, 3u, 4u});
	// Every key still resolves to its own job afterwards.
	CHECK(table.Find(Key(&kDbA, 4, 1.25f)) == otherClip);
	CHECK(table.Find(fading) == otherFade);
}

TEST_CASE("Per-instance node overrides keep their own sample job")
{
	// Two instances in the same clip at the same time, one of them ragdolled: the ragdoll's
	// bones must not leak onto the other, and a bit-identical override set may share.
	std::vector<AnimationContracts::RagdollOverrideEntry> ragdoll(2);
	ragdoll[0].nodeIndex = 5;
	ragdoll[0].transform[3] = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
	ragdoll[1].nodeIndex = 6;
	ragdoll[1].kind = AnimationContracts::kNodeOverrideLocalRotation;
	std::vector<AnimationContracts::RagdollOverrideEntry> moved = ragdoll;
	moved[0].transform[3].y = 1.5f;
	const std::vector<AnimationContracts::RagdollOverrideEntry> sameAsRagdoll = ragdoll;

	DedupeTable<SampleKey> table;
	SampleKey plain = Key(&kDbA, 0, 2.0f);
	SampleKey a = plain;
	a.overrides = ragdoll;
	SampleKey b = plain;
	b.overrides = moved;
	SampleKey c = plain;
	c.overrides = sameAsRagdoll;
	const std::uint32_t plainJob = FindOrAdd(table, plain);
	const std::uint32_t aJob = FindOrAdd(table, a);
	const std::uint32_t bJob = FindOrAdd(table, b);
	CHECK(plainJob != aJob);
	CHECK(aJob != bJob);
	CHECK(FindOrAdd(table, c) == aJob);
	CHECK(table.Size() == 3u);
}

TEST_CASE("Skin jobs are distinct per skin of one sampled state")
{
	DedupeTable<SkinKey> skins;
	const auto add = [&skins](SkinKey k)
	{
		const std::uint32_t found = skins.Find(k);
		return found != DedupeTable<SkinKey>::kNone ? found : skins.Add(k);
	};
	CHECK(add({.sampleJob = 0, .skinIndex = 0}) == 0u);
	CHECK(add({.sampleJob = 0, .skinIndex = 1}) == 1u);
	CHECK(add({.sampleJob = 0, .skinIndex = 0}) == 0u);
	CHECK(add({.sampleJob = 1, .skinIndex = 0}) == 2u);
	CHECK(skins.Size() == 3u);
}

TEST_CASE("Animation scratch counts unique states skins nodes joints and overrides")
{
	std::vector<AnimationContracts::RagdollOverrideEntry> overrides(11);
	SampleKey plain = Key(&kDbA, 0, 1.0f);
	SampleKey ragdoll = plain;
	ragdoll.overrides = overrides;
	std::vector<AnimationScratchDraw> draws(8, {.state = plain, .skinIndex = 0, .nodeCount = 69, .jointCount = 65});
	draws.push_back({.state = plain, .skinIndex = 1, .nodeCount = 69, .jointCount = 12});
	draws.push_back({.state = ragdoll, .skinIndex = 0, .nodeCount = 69, .jointCount = 65});
	draws.push_back(draws.back());
	CHECK(MeasureAnimationScratch(draws) == AnimationScratchCapacity{2, 3, 138, 142, 11});
	CHECK(MeasureAnimationScratch({}) == AnimationScratchCapacity{});
	const AnimationScratchCapacity limits{1024, 1024, 262144, 131072, 2048};
	const auto grown = GrowAnimationScratch({}, MeasureAnimationScratch(draws), limits);
	CHECK(grown == AnimationScratchCapacity{2, 4, 256, 256, 16});
	const std::uint64_t deviceBytes = grown.nodes * (sizeof(AnimationContracts::SampledNodePose) + sizeof(glm::mat4)) + grown.joints * sizeof(glm::mat4);
	CHECK(deviceBytes < 64 * 1024);
	CHECK(GrowAnimationScratch(grown, {}, limits) == grown);
	CHECK(GrowAnimationScratch(grown, {3, 5, 257, 257, 17}, limits) == AnimationScratchCapacity{4, 8, 512, 512, 32});
}

TEST_CASE("Animation scratch growth honors configured caps without wrapping")
{
	const AnimationScratchCapacity limits{3, 5, 1000, 700, 17};
	CHECK(GrowAnimationScratch({}, {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}, limits) == limits);
	CHECK(GrowAnimationScratch({}, {1, 1, 69, 65, 0}, {}) == AnimationScratchCapacity{});
	std::vector<AnimationScratchDraw> draws{
		{.state = Key(&kDbA, 0, 0), .nodeCount = UINT32_MAX, .jointCount = UINT32_MAX},
		{.state = Key(&kDbB, 0, 0), .nodeCount = 69, .jointCount = 65}
	};
	CHECK(MeasureAnimationScratch(draws) == AnimationScratchCapacity{2, 2, UINT32_MAX, UINT32_MAX, 0});
}

TEST_CASE("Animation scratch accommodates growing crowds up to the unchanged animator limit")
{
	const AnimationScratchCapacity limits{1024, 1024, 262144, 131072, 2048};
	AnimationScratchCapacity capacity;
	for (const std::uint32_t actors: {2u, 64u, 1024u})
	{
		std::vector<AnimationScratchDraw> draws;
		for (std::uint32_t actor = 0; actor < actors; ++actor)
		{
			SampleKey state = Key(&kDbA, 0, static_cast<float>(actor));
			state.fadeClip = 1;
			state.fadeTime = static_cast<float>(actor) + 0.25f;
			state.fadeWeight = 0.5f;
			// Body and visible tool share the actor's pose and skin palette.
			draws.push_back({.state = state, .nodeCount = 69, .jointCount = 65});
			draws.push_back(draws.back());
		}
		const auto demand = MeasureAnimationScratch(draws);
		CHECK(demand == AnimationScratchCapacity{actors, actors, actors * 69, actors * 65, 0});
		capacity = GrowAnimationScratch(capacity, demand, limits);
		CHECK(capacity.sampleJobs >= demand.sampleJobs);
		CHECK(capacity.skinJobs >= demand.skinJobs);
		CHECK(capacity.nodes >= demand.nodes);
		CHECK(capacity.joints >= demand.joints);
		CHECK(capacity.sampleJobs <= limits.sampleJobs);
		CHECK(capacity.nodes <= limits.nodes);
		CHECK(capacity.joints <= limits.joints);
		if (actors == 64)
		{
			const auto bytes = static_cast<std::uint64_t>(capacity.nodes) * 112 + static_cast<std::uint64_t>(capacity.joints) * 64;
			CHECK(bytes < 2 * 1024 * 1024);
		}
	}
}

TEST_CASE("Dedupe table survives growth and clears between frames")
{
	DedupeTable<SampleKey> table;
	int db = 0;
	for (std::uint32_t i = 0; i < 2000; ++i)
	{
		CHECK(table.Add(Key(&db, i, 0.0f)) == i);
	}
	for (std::uint32_t i = 0; i < 2000; i += 97)
	{
		CHECK(table.Find(Key(&db, i, 0.0f)) == i);
	}
	table.Clear();
	CHECK(table.Size() == 0u);
	CHECK(table.Find(Key(&db, 5, 0.0f)) == DedupeTable<SampleKey>::kNone);
	CHECK(FindOrAdd(table, Key(&db, 5, 0.0f)) == 0u);
}
