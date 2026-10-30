#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <string_view>
#include <utility>

#include "material/TextureHandle.hpp"
#if !defined(AETHERCORE_WEB_RUNTIME)
#	include "material/Texture.hpp"
#endif
#include "utils/Expected.hpp"

namespace aether
{
	// bare slot. The registry only ever calls GetBindlessSlot(), so it never needs
	class TextureResource
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		TextureResource() = default;
		~TextureResource() = default;

#if !defined(AETHERCORE_WEB_RUNTIME)
		explicit TextureResource(Texture&& texture)
		      : m_texture(std::move(texture)), m_slot(m_texture.GetBindlessSlot())
		{
		}

#endif

		explicit TextureResource(std::uint32_t slot)
		      : m_slot(slot)
		{
		}
#if defined(AETHERCORE_WEB_RUNTIME)
		TextureResource(std::uint32_t slot, std::shared_ptr<void> lifetime)
		      : m_lifetime(std::move(lifetime)), m_slot(slot)
		{
		}
#endif

		TextureResource(const TextureResource&) = delete;
		TextureResource& operator=(const TextureResource&) = delete;
		TextureResource(TextureResource&&) noexcept = default;
		TextureResource& operator=(TextureResource&&) noexcept = default;

		[[nodiscard]] std::uint32_t GetBindlessSlot() const
		{
			return m_slot;
		}

	private:
#if !defined(AETHERCORE_WEB_RUNTIME)
		Texture m_texture;
#endif
#if defined(AETHERCORE_WEB_RUNTIME)
		std::shared_ptr<void> m_lifetime;
#endif
		std::uint32_t m_slot = kInvalidSlot;
	};

#if !defined(AETHERCORE_WEB_RUNTIME)
	struct PreparedTextureUploadSizes
	{
		std::uint64_t stagingBytes = 0;
		std::uint64_t gpuBytes = 0;
	};

	// Keep resource alive until IsPreparedUploadComplete reports completion.
	struct PreparedTextureUpload
	{
		TextureResource resource;
		std::uint64_t ticket = 0;
		std::uint64_t stagingBytes = 0;
		std::uint64_t gpuBytes = 0;
	};
#endif

	class ITextureSlotSink
	{
	public:
		virtual ~ITextureSlotSink() = default;

		[[nodiscard]] virtual std::string ResolvePath(std::string_view path) const = 0;

		[[nodiscard]] virtual Expected<TextureResource> Load(std::string_view resolvedPath, TextureColorSpace colorSpace) = 0;

#if !defined(AETHERCORE_WEB_RUNTIME)
		[[nodiscard]] virtual Expected<PreparedTextureUploadSizes> GetPreparedUploadSizes(const Texture::Prepared& prepared, TextureColorSpace colorSpace) const
		{
			PreparedTextureUploadSizes sizes;
			AE_TRY_VOID(Texture::PreparedAllocationSizes(prepared, "streamed texture", colorSpace, sizes.stagingBytes, sizes.gpuBytes));
			return sizes;
		}
		[[nodiscard]] virtual Expected<PreparedTextureUpload> BeginPreparedUpload(const Texture::Prepared&, std::string_view, TextureColorSpace, std::function<void()> onStagingRetired, std::function<void()> onGpuFailureRetired)
		{
			if (onStagingRetired) onStagingRetired();
			if (onGpuFailureRetired) onGpuFailureRetired();
			AE_UNEXPECTED(AetherError::Engine("Prepared texture upload is unavailable"));
		}
		[[nodiscard]] virtual bool IsPreparedUploadComplete(const PreparedTextureUpload&) const { return false; }
		virtual void WaitPreparedUpload(const PreparedTextureUpload&) {}
		[[nodiscard]] virtual Expected<Texture::ChunkedUpload> BeginChunkedUpload(const Texture::Prepared&, std::string_view, TextureColorSpace, std::function<void()> onGpuFailureRetired, std::function<void()> onGpuFailureBeginRetire)
		{
			if (onGpuFailureBeginRetire) onGpuFailureBeginRetire();
			if (onGpuFailureRetired) onGpuFailureRetired();
			AE_UNEXPECTED(AetherError::Engine("Chunked texture upload is unavailable"));
		}
		[[nodiscard]] virtual Expected<std::uint64_t> PumpChunkedUpload(Texture::ChunkedUpload&, std::uint64_t, const std::function<bool(std::uint64_t)>&, const std::function<void(std::uint64_t)>&)
		{
			AE_UNEXPECTED(AetherError::Engine("Chunked texture upload is unavailable"));
		}
		[[nodiscard]] virtual bool IsChunkedUploadComplete(const Texture::ChunkedUpload&) const { return false; }
		[[nodiscard]] virtual bool IsChunkedUploadIdle(const Texture::ChunkedUpload&) const { return false; }
		virtual void WaitChunkedUpload(const Texture::ChunkedUpload&) {}
#endif

		[[nodiscard]] virtual std::uint32_t Capacity() const = 0;
	};
} // namespace aether
