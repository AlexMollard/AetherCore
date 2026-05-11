#pragma once

#include <filesystem>
#include <string_view>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

#include "vulkan/UniqueImage.hpp"

namespace aether
{
	class BindlessManager;

	// GPU texture loaded from a file (PNG, JPG, BMP, TGA, HDR, …).
	//
	// The image is stored as R8G8B8A8_SRGB so the hardware automatically
	// converts from sRGB storage to linear colour when sampled - which is
	// what we want for albedo textures fed into a linear-light render.
	//
	// After loading, the texture is registered in the BindlessManager and
	// its slot can be placed into a Material for use by the forward shader.
	//
	// Lifetime: call Destroy() (or let move-assign from a default-constructed
	// Texture) before or alongside engine shutdown.
	class Texture
	{
	public:
		Texture() = default;

		Texture(const Texture&) = delete;
		Texture& operator=(const Texture&) = delete;

		Texture(Texture&&) noexcept = default;
		Texture& operator=(Texture&&) noexcept = default;

		// Load an image from disk and upload it to the GPU.
		// uploadQueue + uploadPool are used for a one-time synchronous transfer.
		// The call blocks until the GPU copy is complete.
		[[nodiscard]] static Texture LoadFromFile(std::string_view path, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless, TextureFilter filter = TextureFilter::Linear);

		[[nodiscard]] static Texture LoadFromDiskPath(const std::filesystem::path& path, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless, TextureFilter filter = TextureFilter::Linear);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_image.Get() != VK_NULL_HANDLE;
		}

		[[nodiscard]] std::uint32_t GetBindlessSlot() const;

	private:
		UniqueImage m_image;
	};
} // namespace aether
