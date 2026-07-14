#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "material/ITextureSlotSink.hpp"
#include "material/TextureHandle.hpp"

namespace aether
{
	// Thread-safe: all public methods lock m_mutex. ResourceRegistry /
	class TextureRegistry
	{
	public:
		explicit TextureRegistry(ITextureSlotSink& sink);

		void InitializeDefault(std::string_view fallbackPath);

		// fallbacks so the error texture never itself depends on an asset load.
		void InitializeDefault(TextureResource&& fallback);

		[[nodiscard]] TextureHandle Acquire(std::string_view path);

		void AddRef(TextureHandle handle);

		void Release(TextureHandle handle);

		void ReleaseAll();

		[[nodiscard]] std::uint32_t ResolveSlot(TextureHandle handle) const;

		bool TryGetPath(TextureHandle handle, std::string& outPath) const;

		[[nodiscard]] TextureHandle DefaultHandle() const
		{
			return m_defaultHandle;
		}

	private:
		struct Entry
		{
			std::string resolvedPath;
			std::uint64_t hash = 0;
			TextureResource texture;
			std::uint32_t refcount = 0;
			std::uint32_t generation = 0;
			bool alive = false;
		};

		ITextureSlotSink& m_sink;
		mutable std::mutex m_mutex;
		std::vector<Entry> m_entries;
		std::unordered_multimap<std::uint64_t, std::uint32_t> m_hashToEntry;
		TextureHandle m_defaultHandle{};
		std::uint32_t m_defaultSlot = 0xFFFFFFFFu;
	};
} // namespace aether
