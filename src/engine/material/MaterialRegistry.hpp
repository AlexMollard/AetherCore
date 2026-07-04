#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "material/GpuMaterial.hpp"
#include "material/MaterialHandle.hpp"
#include "material/TextureHandle.hpp"

namespace aether
{
	class IMaterialSlotSink;
	class TextureRegistry;
	struct MaterialAsset;

	// Content-addressed, ref-counted material table over a slot sink. Immutable:
	// identical assets share one slot; per-entity variation is a phase-2 concern.
	// Materials ref-count the textures they reference (cascade through the
	// TextureRegistry), so a texture stays resident while any material uses it.
	class MaterialRegistry
	{
	public:
		MaterialRegistry(IMaterialSlotSink& sink, TextureRegistry& textures);

		// Registers the fallback material returned by ResolveSlot for stale/invalid
		// handles. Call once after construction.
		void InitializeDefault(const MaterialAsset& defaultAsset);

		[[nodiscard]] MaterialHandle Acquire(const MaterialAsset& asset);
		void Release(MaterialHandle handle);
		[[nodiscard]] std::uint32_t ResolveSlot(MaterialHandle handle) const;

		// Reconstructs an authoring asset from a live slot: factors/flags unpacked
		// from the stored GpuMaterial, texture handles copied from the slot's refs
		// (NOT re-AddRef'd - the caller relies on the slot keeping them alive).
		// templateDesc stays default (slots don't record it). False for stale or
		// invalid handles. Powers instance seed-from-current and the inspector.
		bool TryDescribe(MaterialHandle handle, MaterialAsset& out) const;

		[[nodiscard]] MaterialHandle DefaultHandle() const
		{
			return m_defaultHandle;
		}

	private:
		struct SlotEntry
		{
			GpuMaterial packed{};
			std::uint64_t hash = 0;
			std::uint32_t refcount = 0;
			std::uint32_t generation = 0;
			bool alive = false;
			// Texture refs this material slot holds: AddRef'd once on the fresh
			// acquire, dropped once when the slot frees at refcount zero. A dedup-hit
			// Acquire bumps only the material refcount, NOT these (per-slot ref).
			TextureHandle textures[5]{};
		};

		IMaterialSlotSink& m_sink;
		TextureRegistry& m_textures;
		mutable std::mutex m_mutex;
		std::vector<SlotEntry> m_slots;
		std::unordered_multimap<std::uint64_t, std::uint32_t> m_hashToSlot;
		MaterialHandle m_defaultHandle{};
		std::uint32_t m_defaultSlot = 0xFFFFFFFFu;
	};
} // namespace aether
