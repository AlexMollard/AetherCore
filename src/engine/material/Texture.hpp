#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	class BindlessManager;

	// GPU texture loaded from a file (PNG, JPG, BMP, TGA, HDR, ...).
	//
	// The image is stored as R8G8B8A8_SRGB so the hardware automatically
	// converts from sRGB storage to linear colour when sampled - which is
	// what we want for albedo textures fed into a linear-light render.
	//
	// After loading, the texture is registered in the BindlessManager and
	// its slot can be placed into a Material for use by the forward shader.
	//
	// Storage path: stores a single gpu::TextureHandle (8 bytes, typed,
	// generation-checked). Allocated through gpu::ResourceRegistry::
	// CreateTexture which uses the registry's 3-frame deferred-destruction
	// ring.
	//
	// Lifetime: call Destroy() (or let move-assign from a default-constructed
	// Texture) before or alongside engine shutdown.
	class Texture
	{
	public:
		Texture() = default;

		~Texture()
		{
			Destroy();
		}

		Texture(const Texture&) = delete;
		Texture& operator=(const Texture&) = delete;

		Texture(Texture&& other) noexcept
		      : m_handle(std::exchange(other.m_handle, {})), m_bindlessSlot(std::exchange(other.m_bindlessSlot, 0xFFFFFFFFu))
		{
		}

		Texture& operator=(Texture&& other) noexcept
		{
			if (this != &other)
			{
				Destroy();
				m_handle = std::exchange(other.m_handle, {});
				m_bindlessSlot = std::exchange(other.m_bindlessSlot, 0xFFFFFFFFu);
			}
			return *this;
		}

		// Load an image from disk and upload it to the GPU.
		// uploadQueue + uploadPool are used for a one-time synchronous transfer.
		// The call blocks until the GPU copy is complete.
		[[nodiscard]] static Expected<Texture> LoadFromFile(
		        std::string_view path, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter = TextureFilter::Linear);

		// Load a texture from raw file bytes (already read from disk).
		// Useful after an async I/O operation - the GPU upload still happens
		// synchronously on the calling thread (requires a valid Vulkan context).
		[[nodiscard]] static Expected<Texture> LoadFromFileData(std::span<const std::byte> fileData,
		        std::string_view debugPath,
		        gpu::Device device,
		        gpu::Allocator allocator,
		        gpu::Queue uploadQueue,
		        gpu::CommandPool uploadPool,
		        BindlessManager& bindless,
		        TextureFilter filter = TextureFilter::Linear);

		[[nodiscard]] static Expected<Texture> LoadFromDiskPath(
		        const std::filesystem::path& path, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter = TextureFilter::Linear);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_handle.IsValid();
		}

		[[nodiscard]] std::uint32_t GetBindlessSlot() const;

	private:
		gpu::TextureHandle m_handle{};
		std::uint32_t m_bindlessSlot = 0xFFFFFFFFu;
	};
} // namespace aether
