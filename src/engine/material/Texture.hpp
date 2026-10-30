#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/UploadContext.hpp"
#include "material/TextureHandle.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	namespace gpu { class UploadContext; }
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
		struct ChunkedUpload;


		struct Prepared
		{
			std::vector<std::byte> data;
			int width = 0;
			int height = 0;
			bool dds = false;
		};

		[[nodiscard]] static Expected<Prepared> Decode(std::span<const std::byte> data, std::string_view debugPath);
		[[nodiscard]] static Expected<Texture> Upload(const Prepared& prepared, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace = TextureColorSpace::Srgb);
		[[nodiscard]] static Expected<std::uint64_t> PreparedUploadBytes(const Prepared& prepared, std::string_view debugPath, TextureColorSpace colorSpace);
		[[nodiscard]] static Expected<void> PreparedAllocationSizes(const Prepared& prepared, std::string_view debugPath, TextureColorSpace colorSpace, std::uint64_t& stagingBytes, std::uint64_t& gpuBytes);
		// Caller must keep the returned texture alive until the ticket completes.
		[[nodiscard]] static Expected<Texture> UploadAsync(const Prepared& prepared, std::string_view debugPath, gpu::UploadContext& upload, std::uint64_t& ticket, std::uint64_t& bytes, TextureColorSpace colorSpace, std::function<void()> onStagingRetired, std::function<void()> onGpuFailureRetired);
		[[nodiscard]] static Expected<ChunkedUpload> BeginChunkedUpload(const Prepared& prepared, std::string_view debugPath, gpu::UploadContext& upload, TextureColorSpace colorSpace, std::function<void()> onGpuFailureRetired, std::function<void()> onGpuFailureBeginRetire);
		[[nodiscard]] static Expected<std::uint64_t> PumpChunkedUpload(ChunkedUpload& state, std::uint64_t maxBytes, const std::function<bool(std::uint64_t)>& reserveStaging, const std::function<void(std::uint64_t)>& retireStaging);

		[[nodiscard]] static std::string ResolveTexturePath(std::string_view path);

		[[nodiscard]] static Expected<Texture> LoadFromFile(std::string_view path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace = TextureColorSpace::Srgb);
		[[nodiscard]] static Expected<Texture> LoadFromFileData(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace = TextureColorSpace::Srgb);
		[[nodiscard]] static Expected<Texture> LoadFromDiskPath(const std::filesystem::path& path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace = TextureColorSpace::Srgb);

		// missing-texture marker) so they can never themselves fail to load.
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

	struct Texture::ChunkedUpload
	{
		Texture texture;
		gpu::UploadContext* upload = nullptr;
		std::vector<std::byte> pixels;
		std::vector<gpu::ImageUploadRegion> mips;
		std::uint64_t ticket = 0;
		std::uint32_t mip = 0;
		std::uint32_t blockX = 0;
		std::uint32_t blockY = 0;
		std::uint32_t blockWidth = 1;
		std::uint32_t blockHeight = 1;
		std::uint32_t blockBytes = 4;
		bool submitted = false;
		bool complete = false;
	};
} // namespace aether
