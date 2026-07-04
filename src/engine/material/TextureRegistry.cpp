#include "material/TextureRegistry.hpp"

#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		std::uint64_t HashString(std::string_view s)
		{
			// FNV-1a 64-bit, same constants as MaterialRegistry. Collisions
			// resolved by a string-compare on hit, so hash quality only affects
			// dedup speed, not correctness.
			std::uint64_t h = 1469598103934665603ull;
			for (const char c: s)
			{
				h ^= static_cast<unsigned char>(c);
				h *= 1099511628211ull;
			}
			return h;
		}
	} // namespace

	TextureRegistry::TextureRegistry(ITextureSlotSink& sink)
	      : m_sink(sink)
	{
		m_entries.reserve(sink.Capacity());
	}

	void TextureRegistry::InitializeDefault(std::string_view fallbackPath)
	{
		const TextureHandle h = Acquire(fallbackPath);
		std::scoped_lock lock(m_mutex);
		m_defaultHandle = h;
		m_defaultSlot = h.IsValid() ? m_entries[h.index].texture.GetBindlessSlot() : 0xFFFFFFFFu;
	}

	void TextureRegistry::InitializeDefault(TextureResource&& fallback)
	{
		std::scoped_lock lock(m_mutex);
		const std::uint32_t index = static_cast<std::uint32_t>(m_entries.size());
		m_entries.emplace_back();
		Entry& e = m_entries[index];
		// Deliberately NOT indexed in m_hashToEntry: the default is code-owned and
		// must never be a dedup target for a path Acquire. refcount 1 keeps it
		// resident until ReleaseAll (which clears m_entries -> deferred slot free).
		e.resolvedPath = "\x01"
		                 "builtin:default-fallback"; // leading 0x01: never a real VFS path
		e.hash = 0;
		e.texture = std::move(fallback);
		e.refcount = 1;
		e.alive = true;
		m_defaultHandle = TextureHandle{index, e.generation};
		m_defaultSlot = e.texture.GetBindlessSlot();
	}

	TextureHandle TextureRegistry::Acquire(std::string_view path)
	{
		// CRITICAL: resolve BEFORE hashing so two spellings that resolve to the
		// same file share one entry.
		const std::string resolved = m_sink.ResolvePath(path);
		const std::uint64_t hash = HashString(resolved);

		std::scoped_lock lock(m_mutex);

		auto range = m_hashToEntry.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			Entry& e = m_entries[it->second];
			if (e.alive && e.resolvedPath == resolved) // string-compare on hit
			{
				++e.refcount;
				return TextureHandle{it->second, e.generation};
			}
		}

		// Miss: blocking load. On failure (missing/corrupt file, or at capacity)
		// return a BROKEN handle: the caller asked for a texture and could not have
		// it, so packing routes it through ResolveSlot -> the magenta default (a
		// VISIBLE error) instead of silently untextured. AddRef/Release are no-ops
		// on it, so it never touches the default material's refcount - and it is
		// distinct from the invalid handle callers use for "no texture requested".
		auto loaded = m_sink.Load(resolved);
		if (!loaded)
		{
			AE_WARN(LogCategory::Asset, "TextureRegistry: load failed '{}': {}", resolved, loaded.error());
			return TextureHandle::Broken();
		}

		// Reuse a dead slot if one exists (keeps m_entries compact); else append.
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
		std::scoped_lock lock(m_mutex);
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

		std::scoped_lock lock(m_mutex);
		Entry& e = m_entries[handle.index];
		if (!e.alive || e.generation != handle.generation)
		{
			return; // stale/double-free
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
		++e.generation; // invalidate outstanding handles
		e.resolvedPath.clear();
		e.texture = TextureResource{}; // destroys the Texture -> deferred slot free
	}

	void TextureRegistry::ReleaseAll()
	{
		std::scoped_lock lock(m_mutex);
		m_entries.clear(); // destroys each TextureResource -> Texture::Destroy -> deferred free
		m_hashToEntry.clear();
		m_defaultHandle = TextureHandle{};
		m_defaultSlot = 0xFFFFFFFFu;
	}

	std::uint32_t TextureRegistry::ResolveSlot(TextureHandle handle) const
	{
		std::scoped_lock lock(m_mutex);
		if (handle.IsValid() && handle.index < m_entries.size())
		{
			const Entry& e = m_entries[handle.index];
			if (e.alive && e.generation == handle.generation)
			{
				return e.texture.GetBindlessSlot();
			}
		}
		return m_defaultSlot; // stale/invalid -> fallback (visible error)
	}
} // namespace aether
