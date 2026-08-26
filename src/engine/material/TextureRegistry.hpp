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

		// Solid white built-in for content that is INTENTIONALLY untextured
		// (sprites/UI tinted by vertex colour). Distinct from the default so
		// magenta stays an unambiguous "asset load failed" signal.
		void InitializeWhite(TextureResource&& white);

		// colorSpace is part of the cache identity: the same file loaded as colour and
		// as data are two different GPU images, because the format decides whether the
		// sampler decodes sRGB on every fetch.
		[[nodiscard]] TextureHandle Acquire(std::string_view path, TextureColorSpace colorSpace = TextureColorSpace::Srgb);

		void AddRef(TextureHandle handle);

		void Release(TextureHandle handle);

		void ReleaseAll();

		[[nodiscard]] std::uint32_t ResolveSlot(TextureHandle handle) const;

		bool TryGetPath(TextureHandle handle, std::string& outPath) const;

		[[nodiscard]] TextureHandle DefaultHandle() const
		{
			return m_defaultHandle;
		}

		// Falls back to the default (magenta) if InitializeWhite was never called.
		[[nodiscard]] TextureHandle WhiteHandle() const
		{
			return m_whiteHandle.IsValid() ? m_whiteHandle : m_defaultHandle;
		}

	private:
		struct Entry
		{
			std::string resolvedPath;
			std::uint64_t hash = 0;
			TextureColorSpace colorSpace = TextureColorSpace::Srgb;
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
		TextureHandle m_whiteHandle{};
	};
} // namespace aether
