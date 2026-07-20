#include "material/TextureRegistry.hpp"

#include "utils/Hash.hpp"
#include "utils/Logger.hpp"

namespace aether
{
	TextureRegistry::TextureRegistry(ITextureSlotSink& sink)
	      : m_sink(sink)
	{
		m_entries.reserve(sink.Capacity());
	}

	void TextureRegistry::InitializeDefault(std::string_view fallbackPath)
	{
		const TextureHandle h = Acquire(fallbackPath);
		const std::scoped_lock lock(m_mutex);
		m_defaultHandle = h;
		m_defaultSlot = h.IsValid() ? m_entries[h.index].texture.GetBindlessSlot() : 0xFFFFFFFFu;
	}

	void TextureRegistry::InitializeDefault(TextureResource&& fallback)
	{
		const std::scoped_lock lock(m_mutex);
		const std::uint32_t index = static_cast<std::uint32_t>(m_entries.size());
		m_entries.emplace_back();
		Entry& e = m_entries[index];
		// must never be a dedup target for a path Acquire. refcount 1 keeps it
		e.resolvedPath = "\x01"
		                 "builtin:default-fallback"; // leading 0x01: never a real VFS path
		e.hash = 0;
		e.texture = std::move(fallback);
		e.refcount = 1;
		e.alive = true;
		m_defaultHandle = TextureHandle{index, e.generation};
		m_defaultSlot = e.texture.GetBindlessSlot();
	}

	void TextureRegistry::InitializeWhite(TextureResource&& white)
	{
		const std::scoped_lock lock(m_mutex);
		const std::uint32_t index = static_cast<std::uint32_t>(m_entries.size());
		m_entries.emplace_back();
		Entry& e = m_entries[index];
		e.resolvedPath = "\x01"
		                 "builtin:white"; // leading 0x01: never a real VFS path
		e.hash = 0;
		e.texture = std::move(white);
		e.refcount = 1;
		e.alive = true;
		m_whiteHandle = TextureHandle{index, e.generation};
	}

	TextureHandle TextureRegistry::Acquire(std::string_view path)
	{
		const std::string resolved = m_sink.ResolvePath(path);
		const std::uint64_t hash = utils::Fnv1a(resolved);

		const std::scoped_lock lock(m_mutex);

		auto range = m_hashToEntry.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			Entry& e = m_entries[it->second];
			if (e.alive && e.resolvedPath == resolved)
			{
				++e.refcount;
				return TextureHandle{it->second, e.generation};
			}
		}

		// it, so packing routes it through ResolveSlot -> the magenta default (a
		auto loaded = m_sink.Load(resolved);
		if (!loaded)
		{
			AE_WARN(LogCategory::Asset, "TextureRegistry: load failed '{}': {}", resolved, loaded.error());
			return TextureHandle::Broken();
		}

		std::uint32_t index = 0xFFFFFFFFu;
		for (std::uint32_t i = 0; i < m_entries.size(); ++i)
		{
			if (!m_entries[i].alive && m_entries[i].refcount == 0)
			{
				index = i;
				break;
			}
		}
		if (index == 0xFFFFFFFFu)
		{
			index = static_cast<std::uint32_t>(m_entries.size());
			m_entries.emplace_back();
		}

		Entry& e = m_entries[index];
		e.resolvedPath = resolved;
		e.hash = hash;
		e.texture = std::move(*loaded);
		e.refcount = 1;
		e.alive = true;
		m_hashToEntry.emplace(hash, index);
		return TextureHandle{index, e.generation};
	}

	void TextureRegistry::AddRef(TextureHandle handle)
	{
		if (!handle.IsValid() || handle.index >= m_entries.size())
		{
			return;
		}
		const std::scoped_lock lock(m_mutex);
		Entry& e = m_entries[handle.index];
		if (e.alive && e.generation == handle.generation)
		{
			++e.refcount;
		}
	}

	void TextureRegistry::Release(TextureHandle handle)
	{
		if (!handle.IsValid() || handle.index >= m_entries.size())
		{
			return;
		}

		const std::scoped_lock lock(m_mutex);
		Entry& e = m_entries[handle.index];
		if (!e.alive || e.generation != handle.generation)
		{
			return;
		}
		if (--e.refcount > 0)
		{
			return;
		}

		auto range = m_hashToEntry.equal_range(e.hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second == handle.index)
			{
				m_hashToEntry.erase(it);
				break;
			}
		}
		e.alive = false;
		++e.generation;
		e.resolvedPath.clear();
		e.texture = TextureResource{};
	}

	void TextureRegistry::ReleaseAll()
	{
		const std::scoped_lock lock(m_mutex);
		m_entries.clear();
		m_hashToEntry.clear();
		m_defaultHandle = TextureHandle{};
		m_defaultSlot = 0xFFFFFFFFu;
		m_whiteHandle = TextureHandle{};
	}

	std::uint32_t TextureRegistry::ResolveSlot(TextureHandle handle) const
	{
		const std::scoped_lock lock(m_mutex);
		if (handle.IsValid() && handle.index < m_entries.size())
		{
			const Entry& e = m_entries[handle.index];
			if (e.alive && e.generation == handle.generation)
			{
				return e.texture.GetBindlessSlot();
			}
		}
		return m_defaultSlot;
	}

	bool TextureRegistry::TryGetPath(TextureHandle handle, std::string& outPath) const
	{
		const std::scoped_lock lock(m_mutex);
		if (!handle.IsValid() || handle.index >= m_entries.size())
		{
			return false;
		}
		const Entry& e = m_entries[handle.index];
		if (!e.alive || e.generation != handle.generation)
		{
			return false;
		}
		outPath = e.resolvedPath;
		return true;
	}
} // namespace aether
