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

	class MaterialRegistry
	{
	public:
		MaterialRegistry(IMaterialSlotSink& sink, TextureRegistry& textures);

		void InitializeDefault(const MaterialAsset& defaultAsset);

		[[nodiscard]] MaterialHandle Acquire(const MaterialAsset& asset);
		void Release(MaterialHandle handle);
		[[nodiscard]] std::uint32_t ResolveSlot(MaterialHandle handle) const;

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
