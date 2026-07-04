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
	// Ref-counted, PATH-addressed, deduplicated texture table over a slot sink.
	// The entry owns the TextureResource (which owns the bindless slot);
	// handle.index is the ENTRY index; ResolveSlot returns the live heap slot.
	// Diverges from MaterialRegistry (content-addressed) because a texture's
	// identity is its VFS path - you cannot hash the pixels before paying for
	// the decode you are deduping away.
	//
	// Thread-safe: all public methods lock m_mutex. ResourceRegistry /
	// BindlessManager lock internally.
	class TextureRegistry
	{
	public:
		explicit TextureRegistry(ITextureSlotSink& sink);

		// Acquire a fallback texture once; its slot is returned by ResolveSlot for
		// STALE handles (visible error, not silently untextured).
		void InitializeDefault(std::string_view fallbackPath);

		// Install a caller-synthesized fallback (e.g. Texture::CreateSolidColor)
		// as the default, bypassing the sink's path load. Prefer this for built-in
		// fallbacks so the error texture never itself depends on an asset load.
		// Call once, before any Acquire; takes ownership of the resource.
		void InitializeDefault(TextureResource&& fallback);

		[[nodiscard]] TextureHandle Acquire(std::string_view path); // blocking (v1)

		// Bump an already-resident texture's refcount by handle (generation-guarded).
		// The material cascade uses this to add a material-level ref without a path
		// round-trip; Acquire is only for turning a path into a handle.
		void AddRef(TextureHandle handle);

		void Release(TextureHandle handle);

		// Destroy every entry (and its TextureResource -> deferred bindless-slot
		// free). Call on shutdown while the BindlessManager is still alive.
		void ReleaseAll();

		[[nodiscard]] std::uint32_t ResolveSlot(TextureHandle handle) const;

		[[nodiscard]] TextureHandle DefaultHandle() const
		{
			return m_defaultHandle;
		}

	private:
		struct Entry
		{
			std::string resolvedPath; // dedup key (post-resolution)
			std::uint64_t hash = 0;
			TextureResource texture{}; // owns the bindless slot
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
