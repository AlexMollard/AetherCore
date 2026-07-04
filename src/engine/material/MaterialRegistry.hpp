#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "material/GpuMaterial.hpp"
#include "material/MaterialHandle.hpp"

namespace aether
{
	class IMaterialSlotSink;
	struct MaterialAsset;

	// Content-addressed, ref-counted material table over a slot sink. Immutable:
	// identical assets share one slot; per-entity variation is a phase-2 concern.
	class MaterialRegistry
	{
	public:
		explicit MaterialRegistry(IMaterialSlotSink& sink);

		// Registers the fallback material returned by ResolveSlot for stale/invalid
		// handles. Call once after construction.
		void InitializeDefault(const MaterialAsset& defaultAsset);

		[[nodiscard]] MaterialHandle Acquire(const MaterialAsset& asset);
		void Release(MaterialHandle handle);
		[[nodiscard]] std::uint32_t ResolveSlot(MaterialHandle handle) const;
		[[nodiscard]] MaterialHandle DefaultHandle() const { return m_defaultHandle; }

	private:
		struct SlotEntry
		{
			GpuMaterial packed{};
			std::uint64_t hash = 0;
			std::uint32_t refcount = 0;
			std::uint32_t generation = 0;
			bool alive = false;
		};

		IMaterialSlotSink& m_sink;
		mutable std::mutex m_mutex;
		std::vector<SlotEntry> m_slots;
		std::unordered_multimap<std::uint64_t, std::uint32_t> m_hashToSlot;
		MaterialHandle m_defaultHandle{};
		std::uint32_t m_defaultSlot = 0xFFFFFFFFu;
	};
} // namespace aether
