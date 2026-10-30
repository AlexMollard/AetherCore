#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "material/ITextureSlotSink.hpp"

namespace aether
{
	class VulkanContext;

	namespace gpu
	{
		class UploadContext;
	}

	// Texture::LoadFromFile on the game thread (which owns the Vulkan context).
	class AssetTextureSink final : public ITextureSlotSink
	{
	public:
		void Initialize(VulkanContext& context, gpu::UploadContext& upload, std::uint32_t capacity);

		[[nodiscard]] std::string ResolvePath(std::string_view path) const override;
		[[nodiscard]] Expected<TextureResource> Load(std::string_view resolvedPath, TextureColorSpace colorSpace) override;
		[[nodiscard]] Expected<PreparedTextureUpload> BeginPreparedUpload(const Texture::Prepared& prepared, std::string_view path, TextureColorSpace colorSpace, std::function<void()> onStagingRetired, std::function<void()> onGpuFailureRetired) override;
		[[nodiscard]] bool IsPreparedUploadComplete(const PreparedTextureUpload& upload) const override;
		void WaitPreparedUpload(const PreparedTextureUpload& upload) override;
		[[nodiscard]] Expected<Texture::ChunkedUpload> BeginChunkedUpload(const Texture::Prepared& prepared, std::string_view path, TextureColorSpace colorSpace, std::function<void()> onGpuFailureRetired, std::function<void()> onGpuFailureBeginRetire) override;
		[[nodiscard]] Expected<std::uint64_t> PumpChunkedUpload(Texture::ChunkedUpload& state, std::uint64_t maxBytes, const std::function<bool(std::uint64_t)>& reserveStaging, const std::function<void(std::uint64_t)>& retireStaging) override;
		[[nodiscard]] bool IsChunkedUploadComplete(const Texture::ChunkedUpload& state) const override;
		[[nodiscard]] bool IsChunkedUploadIdle(const Texture::ChunkedUpload& state) const override;
		void WaitChunkedUpload(const Texture::ChunkedUpload& state) override;

		[[nodiscard]] std::uint32_t Capacity() const override
		{
			return m_capacity;
		}

	private:
		VulkanContext* m_context = nullptr;
		gpu::UploadContext* m_upload = nullptr;
		std::uint32_t m_capacity = 0;
	};
} // namespace aether
