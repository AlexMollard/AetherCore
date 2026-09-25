#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "rendering/GpuContracts.hpp"

namespace aether::render_queue_anim
{
	// GPU-free job dedupe shared by RenderQueue's prepare pass and its unit tests.
	//
	// A skinned actor is one entity per primitive (a chicken is seven skinned child
	// meshes), and every one of them submits the same animator state. Sampling is a
	// function of that state alone, so one AnimatorSampleJob serves every draw that
	// carries an identical key; the draws then differ only in which skin they copy.

	// Everything an AnimatorSampleJob's output depends on. Fade fields are the job's
	// effective values (zero when the draw is not mid-fade). Overrides are compared by
	// content: a ragdoll's bones or a script's joint offsets are per instance, so two
	// instances only share a job when their overrides are bit-identical too - in which
	// case the sampled poses are identical as well.
	struct SampleKey
	{
		const void* db = nullptr;
		std::uint32_t dbGeneration = 0;
		std::uint32_t clip = 0;
		float time = 0.0f;
		std::uint32_t fadeClip = 0;
		float fadeTime = 0.0f;
		float fadeWeight = 0.0f;
		std::span<const AnimationContracts::RagdollOverrideEntry> overrides;

		[[nodiscard]] bool operator==(const SampleKey& o) const
		{
			// Floats compare by bits: a key is "the same input", not "a close input".
			return db == o.db && dbGeneration == o.dbGeneration && clip == o.clip && std::bit_cast<std::uint32_t>(time) == std::bit_cast<std::uint32_t>(o.time) && fadeClip == o.fadeClip &&
			       std::bit_cast<std::uint32_t>(fadeTime) == std::bit_cast<std::uint32_t>(o.fadeTime) && std::bit_cast<std::uint32_t>(fadeWeight) == std::bit_cast<std::uint32_t>(o.fadeWeight) &&
			       overrides.size() == o.overrides.size() && (overrides.empty() || std::memcmp(overrides.data(), o.overrides.data(), overrides.size_bytes()) == 0);
		}

		[[nodiscard]] std::uint64_t Hash() const
		{
			std::uint64_t h = 0xcbf29ce484222325ull;
			const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
			mix(reinterpret_cast<std::uintptr_t>(db));
			mix((static_cast<std::uint64_t>(dbGeneration) << 32) | clip);
			mix((static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(time)) << 32) | fadeClip);
			mix((static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(fadeTime)) << 32) | std::bit_cast<std::uint32_t>(fadeWeight));
			mix(overrides.size());
			for (const AnimationContracts::RagdollOverrideEntry& ov: overrides)
			{
				mix((static_cast<std::uint64_t>(ov.kind) << 32) | ov.nodeIndex);
				for (int c = 0; c < 4; ++c)
				{
					mix((static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(ov.transform[c].x)) << 32) | std::bit_cast<std::uint32_t>(ov.transform[c].y));
					mix((static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(ov.transform[c].z)) << 32) | std::bit_cast<std::uint32_t>(ov.transform[c].w));
				}
			}
			return h;
		}
	};

	// One skin palette built from one sampled pose.
	struct SkinKey
	{
		std::uint32_t sampleJob = 0;
		std::uint32_t skinIndex = 0;

		[[nodiscard]] bool operator==(const SkinKey&) const = default;

		[[nodiscard]] std::uint64_t Hash() const
		{
			return ((static_cast<std::uint64_t>(sampleJob) << 32) | skinIndex) * 0x9e3779b97f4a7c15ull;
		}
	};

	// Per-frame insertion-ordered set: the n-th distinct key added gets index n. Open
	// addressing over index slots, so Clear() keeps every allocation and a steady-state
	// frame allocates nothing.
	template<class Key>
	class DedupeTable
	{
	public:
		static constexpr std::uint32_t kNone = UINT32_MAX;

		void Clear()
		{
			m_keys.clear();
			std::fill(m_slots.begin(), m_slots.end(), kNone);
		}

		[[nodiscard]] std::uint32_t Size() const
		{
			return static_cast<std::uint32_t>(m_keys.size());
		}

		// Index of an equal key already added this frame, or kNone.
		[[nodiscard]] std::uint32_t Find(const Key& key) const
		{
			if (m_slots.empty())
			{
				return kNone;
			}
			const std::size_t mask = m_slots.size() - 1;
			for (std::size_t s = static_cast<std::size_t>(key.Hash()) & mask;; s = (s + 1) & mask)
			{
				const std::uint32_t idx = m_slots[s];
				if (idx == kNone || m_keys[idx] == key)
				{
					return idx;
				}
			}
		}

		// Adds a key Find() did not return and gives it the next index.
		std::uint32_t Add(const Key& key)
		{
			if ((m_keys.size() + 1) * 2 > m_slots.size())
			{
				Rehash(m_slots.empty() ? 256 : m_slots.size() * 2);
			}
			const auto idx = static_cast<std::uint32_t>(m_keys.size());
			m_keys.push_back(key);
			Insert(idx);
			return idx;
		}

	private:
		void Insert(std::uint32_t idx)
		{
			const std::size_t mask = m_slots.size() - 1;
			std::size_t s = static_cast<std::size_t>(m_keys[idx].Hash()) & mask;
			while (m_slots[s] != kNone)
			{
				s = (s + 1) & mask;
			}
			m_slots[s] = idx;
		}

		void Rehash(std::size_t slotCount)
		{
			m_slots.assign(slotCount, kNone);
			for (std::uint32_t i = 0; i < m_keys.size(); ++i)
			{
				Insert(i);
			}
		}

		std::vector<Key> m_keys;
		std::vector<std::uint32_t> m_slots;
	};
} // namespace aether::render_queue_anim
