#include <doctest/doctest.h>
#include "rendering/AnimJobDedupe.hpp"

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
