#include "material/MaterialRegistry.hpp"

#include <cstring>

#include "material/IMaterialSlotSink.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialPacking.hpp"
#include "material/TextureRegistry.hpp"

namespace aether
{
	namespace
	{
		std::uint64_t HashBytes(const void* data, std::size_t n)
		{
			// FNV-1a 64-bit. Collisions are resolved by a byte-compare on hit,
			// so hash quality only affects dedup speed, not correctness.
			const auto* p = static_cast<const unsigned char*>(data);
			std::uint64_t h = 1469598103934665603ull;
			for (std::size_t i = 0; i < n; ++i)
			{
				h ^= p[i];
				h *= 1099511628211ull;
			}
			return h;
		}
	} // namespace

	MaterialRegistry::MaterialRegistry(IMaterialSlotSink& sink, TextureRegistry& textures)
	      : m_sink(sink), m_textures(textures)
	{
		m_slots.resize(sink.Capacity());
	}

	void MaterialRegistry::InitializeDefault(const MaterialAsset& defaultAsset)
	{
		const MaterialHandle h = Acquire(defaultAsset);
		std::scoped_lock lock(m_mutex);
		m_defaultHandle = h;
		m_defaultSlot = h.index;
	}

	MaterialHandle MaterialRegistry::Acquire(const MaterialAsset& asset)
	{
		const GpuMaterial packed = PackMaterial(asset, m_textures);
		const std::uint64_t hash = HashBytes(&packed, sizeof(packed));

		std::scoped_lock lock(m_mutex);

		auto range = m_hashToSlot.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			const std::uint32_t slot = it->second;
			SlotEntry& e = m_slots[slot];
			if (e.alive && std::memcmp(&e.packed, &packed, sizeof(packed)) == 0)
			{
				// Dedup hit: another material handle shares this slot. The slot
				// already holds one material-level ref on its textures; do NOT bump
				// them again (the texture ref is per material-SLOT, dropped once when
				// the slot frees at refcount zero).
				++e.refcount;
				return MaterialHandle{slot, e.generation};
			}
		}

		const std::uint32_t slot = m_sink.AllocateSlot();
		if (slot == IMaterialSlotSink::kInvalidSlot || slot >= m_slots.size())
		{
			// Sink full: return an invalid handle. ResolveSlot() falls back to the
			// default slot for it, and Release() is a safe no-op - so this never
			// touches the default material's refcount (returning m_defaultHandle
			// here would let the caller's later Release() free the default).
			return MaterialHandle{};
		}

		m_sink.Write(slot, packed);
		SlotEntry& e = m_slots[slot];
		e.packed = packed;
		e.hash = hash;
		e.refcount = 1;
		e.alive = true;
		// Take a material-level ref on each texture this material uses, and record
		// the handles so Release drops exactly these.
		const TextureHandle assetTextures[5] = {asset.albedoTex, asset.normalTex, asset.metallicRoughnessTex, asset.occlusionTex, asset.emissiveTex};
		for (int i = 0; i < 5; ++i)
		{
			e.textures[i] = assetTextures[i];
			if (assetTextures[i].IsValid())
			{
				m_textures.AddRef(assetTextures[i]);
			}
		}
		m_hashToSlot.emplace(hash, slot);
		return MaterialHandle{slot, e.generation};
	}

	void MaterialRegistry::Release(MaterialHandle handle)
	{
		if (!handle.IsValid() || handle.index >= m_slots.size())
		{
			return;
		}

		std::scoped_lock lock(m_mutex);
		SlotEntry& e = m_slots[handle.index];
		if (!e.alive || e.generation != handle.generation)
		{
			return; // stale/double-free
		}
		if (--e.refcount > 0)
		{
			return;
		}

		auto range = m_hashToSlot.equal_range(e.hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second == handle.index)
			{
				m_hashToSlot.erase(it);
				break;
			}
		}
		// Drop this material's texture refs (cascade). Balanced against the single
		// AddRef taken on the fresh-acquire path.
		for (int i = 0; i < 5; ++i)
		{
			if (e.textures[i].IsValid())
			{
				m_textures.Release(e.textures[i]);
				e.textures[i] = TextureHandle{};
			}
		}
		e.alive = false;
		++e.generation; // invalidate outstanding handles to this slot
		m_sink.FreeSlot(handle.index);
	}

	std::uint32_t MaterialRegistry::ResolveSlot(MaterialHandle handle) const
	{
		std::scoped_lock lock(m_mutex);
		if (handle.IsValid() && handle.index < m_slots.size())
		{
			const SlotEntry& e = m_slots[handle.index];
			if (e.alive && e.generation == handle.generation)
			{
				return handle.index;
			}
		}
		return m_defaultSlot;
	}

	bool MaterialRegistry::TryDescribe(MaterialHandle handle, MaterialAsset& out) const
	{
		if (!handle.IsValid() || handle.index >= m_slots.size())
		{
			return false;
		}
		std::scoped_lock lock(m_mutex);
		const SlotEntry& e = m_slots[handle.index];
		if (!e.alive || e.generation != handle.generation)
		{
			return false;
		}

		const GpuMaterial& g = e.packed;
		out = MaterialAsset{};
		out.baseColorFactor = g.baseColorFactor;
		out.metallicFactor = g.metallicFactor;
		out.roughnessFactor = g.roughnessFactor;
		out.occlusionStrength = g.occlusionStrength;
		out.alphaCutoff = g.alphaCutoff;
		out.emissiveFactor = glm::vec3(g.emissiveFactor);
		out.doubleSided = (g.flags & GpuMaterial::kDoubleSided) != 0;
		out.alphaBlend = (g.flags & GpuMaterial::kAlphaBlend) != 0;
		out.alphaMask = (g.flags & GpuMaterial::kAlphaMask) != 0;
		out.modulateVertexColor = (g.flags & GpuMaterial::kModulateVertexColor) != 0;
		// Same order the acquire cascade recorded them in.
		out.albedoTex = e.textures[0];
		out.normalTex = e.textures[1];
		out.metallicRoughnessTex = e.textures[2];
		out.occlusionTex = e.textures[3];
		out.emissiveTex = e.textures[4];
		return true;
	}
} // namespace aether
