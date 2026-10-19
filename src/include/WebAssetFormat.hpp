#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

inline constexpr const char* AE_WEB_ASSET_TARGET = "webgpu-2d-v1";
inline constexpr std::uint32_t AE_WEB_TEXTURE_VERSION = 1;

struct WebTextureHeader
{
	char magic[4] = {'A', 'E', 'R', '8'};
	std::uint32_t version = AE_WEB_TEXTURE_VERSION;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t format = 1; // RGBA8 unorm, tightly packed rows
	std::uint32_t reserved = 0;
	std::uint64_t pixelBytes = 0;
};

static_assert(sizeof(WebTextureHeader) == 32);

inline bool ReadWebTextureHeader(std::span<const std::byte> bytes, WebTextureHeader& header)
{
	if (std::endian::native != std::endian::little || bytes.size() < sizeof(header))
	{
		return false;
	}
	std::memcpy(&header, bytes.data(), sizeof(header));
	return std::memcmp(header.magic, "AER8", 4) == 0 && header.version == AE_WEB_TEXTURE_VERSION && header.format == 1 && header.reserved == 0 && header.width > 0 && header.height > 0 && header.width <= 4096 && header.height <= 4096
	       && header.pixelBytes == std::uint64_t(header.width) * header.height * 4 && header.pixelBytes == bytes.size() - sizeof(header);
}
