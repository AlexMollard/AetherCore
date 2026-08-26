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
