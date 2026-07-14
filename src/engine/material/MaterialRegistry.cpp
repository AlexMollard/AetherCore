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
		const std::scoped_lock lock(m_mutex);
		m_defaultHandle = h;
		m_defaultSlot = h.index;
	}

	MaterialHandle MaterialRegistry::Acquire(const MaterialAsset& asset)
	{
		const GpuMaterial packed = PackMaterial(asset, m_textures);
		const std::uint64_t hash = HashBytes(&packed, sizeof(packed));

		const std::scoped_lock lock(m_mutex);

		auto range = m_hashToSlot.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			const std::uint32_t slot = it->second;
			SlotEntry& e = m_slots[slot];
			if (e.alive && e.packed == packed)
			{
				++e.refcount;
				return MaterialHandle{slot, e.generation};
			}
		}

		const std::uint32_t slot = m_sink.AllocateSlot();
		if (slot == IMaterialSlotSink::kInvalidSlot || slot >= m_slots.size())
		{
			// default slot for it, and Release() is a safe no-op - so this never
			return MaterialHandle{};
		}

		m_sink.Write(slot, packed);
		SlotEntry& e = m_slots[slot];
		e.packed = packed;
		e.hash = hash;
		e.refcount = 1;
		e.alive = true;
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

		const std::scoped_lock lock(m_mutex);
		SlotEntry& e = m_slots[handle.index];
		if (!e.alive || e.generation != handle.generation)
		{
			return;
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
		for (auto& texture: e.textures)
		{
			if (texture.IsValid())
			{
				m_textures.Release(texture);
				texture = TextureHandle{};
			}
		}
		e.alive = false;
		++e.generation;
		m_sink.FreeSlot(handle.index);
	}

	std::uint32_t MaterialRegistry::ResolveSlot(MaterialHandle handle) const
	{
		const std::scoped_lock lock(m_mutex);
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
		const std::scoped_lock lock(m_mutex);
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
		out.albedoTex = e.textures[0];
		out.normalTex = e.textures[1];
		out.metallicRoughnessTex = e.textures[2];
		out.occlusionTex = e.textures[3];
		out.emissiveTex = e.textures[4];
		return true;
	}
} // namespace aether
