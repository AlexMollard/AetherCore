#include "material/TextureRegistry.hpp"

#include "io/FileSystem.hpp"
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
		// A failed load yields TextureHandle::Broken(), which passes IsValid() (so
		// packing still routes through ResolveSlot) but is no entry index - the same
		// bounds check every other handle consumer here applies.
		m_defaultSlot = h.IsValid() && h.index < m_entries.size() ? m_entries[h.index].texture.GetBindlessSlot() : 0xFFFFFFFFu;
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

	TextureHandle TextureRegistry::Acquire(std::string_view path, TextureColorSpace colorSpace)
	{
		std::string reference = path.starts_with("uid://") ? std::string(path) : io::FileSystem::AssetReference(path);
		const std::string source = io::FileSystem::ResolveAssetPath(reference);
		if (source.empty())
			return TextureHandle::Broken();
		const std::string resolved = m_sink.ResolvePath(source);
		if (!reference.starts_with("uid://"))
		{
			reference = resolved;
		}
		const std::uint64_t hash = utils::Fnv1a(resolved) ^ (colorSpace == TextureColorSpace::Linear ? 0x9e3779b97f4a7c15ull : 0ull);

		const std::scoped_lock lock(m_mutex);

		auto range = m_hashToEntry.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			Entry& e = m_entries[it->second];
			if (e.alive && e.colorSpace == colorSpace && e.resolvedPath == resolved)
			{
				if (reference.starts_with("uid://"))
				{
					e.reference = reference;
				}
				++e.refcount;
				return TextureHandle{it->second, e.generation};
			}
		}

		// it, so packing routes it through ResolveSlot -> the magenta default (a
		auto loaded = m_sink.Load(resolved, colorSpace);
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
		e.reference = reference;
		e.hash = hash;
		e.colorSpace = colorSpace;
		e.texture = std::move(*loaded);
		e.refcount = 1;
		e.alive = true;
		m_hashToEntry.emplace(hash, index);
		return TextureHandle{index, e.generation};
	}

	void TextureRegistry::AddRef(TextureHandle handle)
	{
		const std::scoped_lock lock(m_mutex);
		if (!handle.IsValid() || handle.index >= m_entries.size())
		{
			return;
		}
		Entry& e = m_entries[handle.index];
		if (e.alive && e.generation == handle.generation)
		{
			++e.refcount;
		}
	}

	void TextureRegistry::Release(TextureHandle handle)
	{
		const std::scoped_lock lock(m_mutex);
		if (!handle.IsValid() || handle.index >= m_entries.size())
		{
			return;
		}
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
		e.reference.clear();
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
		// Builtin entries carry a reserved 0x01-prefixed "path" that is by
		// construction never a real VFS path; serializing it bakes an unloadable
		// path into the file, so a white/builtin load degrades to magenta after one
		// save round-trip. Report no path so callers serialize "no texture".
		if (!e.resolvedPath.empty() && e.resolvedPath.front() == '\x01')
		{
			return false;
		}
		outPath = e.reference.empty() ? e.resolvedPath : e.reference;
		return true;
	}
	void TextureRegistry::RetargetPaths(std::string_view oldVfs, std::string_view newVfs)
	{
		const std::scoped_lock lock(m_mutex);
		for (std::uint32_t index = 0; index < m_entries.size(); ++index)
		{
			auto& e = m_entries[index];
			if (!e.alive || e.resolvedPath.starts_with("\x01"))
			{
				continue;
			}
			const auto matches = [](std::string_view path, std::string_view prefix)
			{
				return path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() && path[prefix.size()] == '/');
			};
			std::string newPath;
			if (e.reference.starts_with("uid://"))
			{
				newPath = io::FileSystem::ResolveAssetPath(e.reference);
				if (!matches(newPath, newVfs))
				{
					continue;
				}
			}
			else if (matches(e.reference, oldVfs))
			{
				newPath = std::string(newVfs) + e.reference.substr(oldVfs.size());
			}
			else if (e.resolvedPath == m_sink.ResolvePath(oldVfs))
			{
				newPath = std::string(newVfs);
			}
			else
			{
				continue;
			}
			auto range = m_hashToEntry.equal_range(e.hash);
			for (auto it = range.first; it != range.second; ++it)
			{
				if (it->second == index)
				{
					m_hashToEntry.erase(it);
					break;
				}
			}
			if (!e.reference.starts_with("uid://"))
			{
				const auto reference = io::FileSystem::AssetReference(newPath);
				e.reference = reference.starts_with("uid://") ? reference : m_sink.ResolvePath(newPath);
			}
			e.resolvedPath = m_sink.ResolvePath(newPath);
			e.hash = utils::Fnv1a(e.resolvedPath) ^ (e.colorSpace == TextureColorSpace::Linear ? 0x9e3779b97f4a7c15ull : 0ull);
			m_hashToEntry.emplace(e.hash, index);
		}
	}
} // namespace aether
