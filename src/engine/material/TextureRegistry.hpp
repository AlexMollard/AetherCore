#pragma once

#include <cstdint>
#include <functional>
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

		// Acquire an already resident path without performing any IO or decoding.
		[[nodiscard]] TextureHandle FindAndAcquire(std::string_view path, TextureColorSpace colorSpace = TextureColorSpace::Srgb);
		// Retain the canonical owner of an extracted bindless slot, if any.
		[[nodiscard]] TextureHandle FindAndAcquireSlot(std::uint32_t slot);

		// Publish an already uploaded resource under the same identity as Acquire.
		// The returned handle owns one reference; a duplicate resource is discarded.
		// Begin-retire runs when this resource leaves the registry (or is discarded);
		// onTextureRetired runs after its deferred physical destruction. Callbacks
		// belong to the supplied resource, never to an existing shared entry.
		[[nodiscard]] TextureHandle PublishPrepared(std::string_view path, TextureColorSpace colorSpace, TextureResource texture, std::function<void()> onTextureRetired = {}, std::function<void()> onTextureBeginRetire = {});
		static void RetirePrepared(TextureResource texture, std::function<void()> onTextureRetired, std::function<void()> onTextureBeginRetire = {});

#if !defined(AETHERCORE_WEB_RUNTIME)
		[[nodiscard]] Expected<PreparedTextureUploadSizes> GetPreparedUploadSizes(const Texture::Prepared& prepared, TextureColorSpace colorSpace) const;
		[[nodiscard]] Expected<PreparedTextureUpload> BeginPreparedUpload(const Texture::Prepared& prepared, std::string_view path, TextureColorSpace colorSpace, std::function<void()> onStagingRetired = {}, std::function<void()> onGpuFailureRetired = {});
		[[nodiscard]] bool IsPreparedUploadComplete(const PreparedTextureUpload& upload) const;
		void WaitPreparedUpload(const PreparedTextureUpload& upload);
		[[nodiscard]] Expected<std::uint64_t> GetPreparedUploadBytes(const Texture::Prepared& prepared, TextureColorSpace colorSpace) const;
		[[nodiscard]] Expected<Texture::ChunkedUpload> BeginChunkedUpload(const Texture::Prepared& prepared, std::string_view path, TextureColorSpace colorSpace, std::function<void()> onGpuFailureRetired, std::function<void()> onGpuFailureBeginRetire = {});
		[[nodiscard]] Expected<std::uint64_t> PumpChunkedUpload(Texture::ChunkedUpload& state, std::uint64_t maxBytes, const std::function<bool(std::uint64_t)>& reserveStaging, const std::function<void(std::uint64_t)>& retireStaging);
		[[nodiscard]] bool IsChunkedUploadComplete(const Texture::ChunkedUpload& state) const;
		[[nodiscard]] bool IsChunkedUploadIdle(const Texture::ChunkedUpload& state) const;
		void WaitChunkedUpload(const Texture::ChunkedUpload& state);
#endif

		void AddRef(TextureHandle handle);

		void Release(TextureHandle handle);

		void ReleaseAll();

		[[nodiscard]] std::uint32_t ResolveSlot(TextureHandle handle) const;

		bool TryGetPath(TextureHandle handle, std::string& outPath) const;
		// Logical UTF-8 byte count, excluding the terminator; same validity rules as TryGetPath.
		bool TryGetPathSize(TextureHandle handle, std::size_t& outBytes) const;

		// Retarget live path-backed entries without invalidating texture handles.
		void RetargetPaths(std::string_view oldVfs, std::string_view newVfs);

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
			std::string reference;
			std::uint64_t hash = 0;
			TextureColorSpace colorSpace = TextureColorSpace::Srgb;
			TextureResource texture;
			std::function<void()> onTextureBeginRetire;
			std::function<void()> onTextureRetired;
			std::uint32_t refcount = 0;
			std::uint32_t generation = 0;
			bool alive = false;
		};

		[[nodiscard]] TextureHandle FindLocked(const std::string& resolved, const std::string& reference, TextureColorSpace colorSpace, std::uint64_t hash);
		[[nodiscard]] TextureHandle InsertLocked(std::string resolved, std::string reference, TextureColorSpace colorSpace, std::uint64_t hash, TextureResource&& texture);

		ITextureSlotSink& m_sink;
		mutable std::mutex m_mutex;
		std::vector<Entry> m_entries;
		std::unordered_multimap<std::uint64_t, std::uint32_t> m_hashToEntry;
		TextureHandle m_defaultHandle{};
		std::uint32_t m_defaultSlot = 0xFFFFFFFFu;
		TextureHandle m_whiteHandle{};
	};
} // namespace aether
