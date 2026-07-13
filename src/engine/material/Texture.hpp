#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
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

		// Resolve a texture VFS path to the canonical file that will actually be
		// loaded: the pre-transcoded .texture sibling if it exists, else the path
		// itself. Shared by LoadFromFile and the TextureRegistry's sink so the
		// registry dedups on the exact string LoadFromFile reads.
		[[nodiscard]] static std::string ResolveTexturePath(std::string_view path);

		[[nodiscard]] static Expected<Texture> LoadFromFile(std::string_view path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool);
		[[nodiscard]] static Expected<Texture> LoadFromFileData(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool);
		[[nodiscard]] static Expected<Texture> LoadFromDiskPath(const std::filesystem::path& path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool);

		// Synthesize a 1x1 solid-colour texture (bindless, sampled) with no file
		// I/O or decode. Used for in-binary built-in fallbacks (e.g. the magenta
		// missing-texture marker) so they can never themselves fail to load.
		// rgba is straight RGBA bytes uploaded to an R8G8B8A8_SRGB image.
		[[nodiscard]] static Expected<Texture> CreateSolidColor(std::array<std::uint8_t, 4> rgba, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_handle.IsValid();
		}

		[[nodiscard]] gpu::ImageView GetView() const;
		[[nodiscard]] std::uint32_t GetBindlessSlot() const;

	private:
		gpu::TextureHandle m_handle{};
		std::uint32_t m_bindlessSlot = 0xFFFFFFFFu;
	};
} // namespace aether
