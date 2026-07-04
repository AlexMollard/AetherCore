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
		// Same resolution LoadFromFile uses, so the registry dedups on the exact
		// string that will actually be read.
		return Texture::ResolveTexturePath(path);
	}

	Expected<TextureResource> AssetTextureSink::Load(std::string_view resolvedPath)
	{
		AE_TRY(tex, Texture::LoadFromFile(resolvedPath, m_context->GetDevice().device, m_context->GetGraphicsQueue(), m_upload->GetCommandPool()));
		return TextureResource{std::move(*tex)};
	}
} // namespace aether
