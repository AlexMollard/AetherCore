#include "material/AssetTextureSink.hpp"

#include "gpu/UploadContext.hpp"
#include "material/Texture.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void AssetTextureSink::Initialize(VulkanContext& context, gpu::UploadContext& upload, std::uint32_t capacity)
	{
		m_context = &context;
		m_upload = &upload;
		m_capacity = capacity;
	}

	std::string AssetTextureSink::ResolvePath(std::string_view path) const
	{
		return Texture::ResolveTexturePath(path);
	}

	Expected<TextureResource> AssetTextureSink::Load(std::string_view resolvedPath, TextureColorSpace colorSpace)
	{
		AE_TRY(tex, Texture::LoadFromFile(resolvedPath, m_context->GetDevice().device, m_context->GetGraphicsQueue(), m_upload->GetCommandPool(), colorSpace));
		return TextureResource{std::move(*tex)};
	}

	Expected<PreparedTextureUpload> AssetTextureSink::BeginPreparedUpload(const Texture::Prepared& prepared, std::string_view path, TextureColorSpace colorSpace, std::function<void()> onStagingRetired, std::function<void()> onGpuFailureRetired)
	{
		auto sizes = GetPreparedUploadSizes(prepared, colorSpace);
		if (!sizes)
		{
			if (onStagingRetired) onStagingRetired();
			if (onGpuFailureRetired) onGpuFailureRetired();
			return std::unexpected(std::move(sizes.error()));
		}
		std::uint64_t ticket = 0;
		std::uint64_t bytes = 0;
		AE_TRY(texture, Texture::UploadAsync(prepared, path, *m_upload, ticket, bytes, colorSpace, std::move(onStagingRetired), std::move(onGpuFailureRetired)));
		return PreparedTextureUpload{TextureResource{std::move(*texture)}, ticket, sizes->stagingBytes, sizes->gpuBytes};
	}

	bool AssetTextureSink::IsPreparedUploadComplete(const PreparedTextureUpload& upload) const
	{
		const bool complete = m_upload->IsUploadComplete(upload.ticket);
		if (complete)
			m_upload->CollectAsyncUploads();
		return complete;
	}

	void AssetTextureSink::WaitPreparedUpload(const PreparedTextureUpload& upload)
	{
		m_upload->WaitForUpload(upload.ticket);
		m_upload->CollectAsyncUploads();
	}

	Expected<Texture::ChunkedUpload> AssetTextureSink::BeginChunkedUpload(const Texture::Prepared& prepared, std::string_view path, TextureColorSpace colorSpace, std::function<void()> onGpuFailureRetired, std::function<void()> onGpuFailureBeginRetire)
	{
		return Texture::BeginChunkedUpload(prepared, path, *m_upload, colorSpace, std::move(onGpuFailureRetired), std::move(onGpuFailureBeginRetire));
	}
	Expected<std::uint64_t> AssetTextureSink::PumpChunkedUpload(Texture::ChunkedUpload& state, std::uint64_t maxBytes, const std::function<bool(std::uint64_t)>& reserveStaging, const std::function<void(std::uint64_t)>& retireStaging)
	{
		m_upload->CollectAsyncUploads();
		return Texture::PumpChunkedUpload(state, maxBytes, reserveStaging, retireStaging);
	}
	bool AssetTextureSink::IsChunkedUploadIdle(const Texture::ChunkedUpload& state) const
	{
		const bool idle = !state.ticket || m_upload->IsUploadComplete(state.ticket);
		m_upload->CollectAsyncUploads();
		return idle;
	}
	bool AssetTextureSink::IsChunkedUploadComplete(const Texture::ChunkedUpload& state) const
	{
		return state.complete && IsChunkedUploadIdle(state);
	}
	void AssetTextureSink::WaitChunkedUpload(const Texture::ChunkedUpload& state)
	{
		if (state.ticket) m_upload->WaitForUpload(state.ticket);
		m_upload->CollectAsyncUploads();
	}
} // namespace aether
