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

		[[nodiscard]] static Expected<Texture> LoadFromFile(std::string_view path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool);
		[[nodiscard]] static Expected<Texture> LoadFromFileData(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool);
		[[nodiscard]] static Expected<Texture> LoadFromDiskPath(const std::filesystem::path& path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool);

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
