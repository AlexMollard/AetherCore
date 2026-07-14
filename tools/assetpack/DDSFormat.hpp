#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aether::assetpipeline
{

	inline constexpr uint32_t DDS_MAGIC = 0x20534444u;

	inline constexpr uint32_t DDSD_CAPS = 0x00000001u;
	inline constexpr uint32_t DDSD_HEIGHT = 0x00000002u;
	inline constexpr uint32_t DDSD_WIDTH = 0x00000004u;
	inline constexpr uint32_t DDSD_PIXELFORMAT = 0x00001000u;
	inline constexpr uint32_t DDSD_LINEARSIZE = 0x00080000u;
	inline constexpr uint32_t DDSD_MIPMAPCOUNT = 0x00020000u;

	inline constexpr uint32_t DDPF_FOURCC = 0x00000004u;

	inline constexpr uint32_t DDSCAPS_TEXTURE = 0x00001000u;
	inline constexpr uint32_t DDSCAPS_COMPLEX = 0x00000008u;
	inline constexpr uint32_t DDSCAPS_MIPMAP = 0x00400000u;

	inline constexpr uint32_t FOURCC_DX10 = 0x30315844u;

	inline constexpr uint32_t DXGI_FORMAT_BC4_UNORM = 80u;
	inline constexpr uint32_t DXGI_FORMAT_BC7_UNORM = 98u;
	inline constexpr uint32_t DXGI_FORMAT_BC7_UNORM_SRGB = 99u;

	inline constexpr uint32_t D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3u;

	// ── DDS on-disk structures ──────────────────────────────────────────────

#pragma pack(push, 1)

	struct DDSPixelFormat
	{
		uint32_t dwSize = 32;
		uint32_t dwFlags = DDPF_FOURCC;
		uint32_t dwFourCC = FOURCC_DX10;
		uint32_t dwRGBBitCount = 0;
		uint32_t dwRBitMask = 0;
		uint32_t dwGBitMask = 0;
		uint32_t dwBBitMask = 0;
		uint32_t dwABitMask = 0;
	};

	struct DDSHeader
	{
		uint32_t dwSize = 124;
		uint32_t dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
		uint32_t dwHeight = 0;
		uint32_t dwWidth = 0;
		uint32_t dwPitchOrLinearSize = 0;
		uint32_t dwDepth = 0;
		uint32_t dwMipMapCount = 0;
		uint32_t dwReserved1[11] = {};
		DDSPixelFormat ddspf;
		uint32_t dwCaps = DDSCAPS_TEXTURE;
		uint32_t dwCaps2 = 0;
		uint32_t dwCaps3 = 0;
		uint32_t dwCaps4 = 0;
		uint32_t dwReserved2 = 0;
	};

	struct DDSHeaderDXT10
	{
		uint32_t dxgiFormat = 0;
		uint32_t resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
		uint32_t miscFlag = 0;
		uint32_t arraySize = 1;
		uint32_t miscFlags2 = 0;
	};

#pragma pack(pop)

	static_assert(sizeof(DDSHeader) == 124);
	static_assert(sizeof(DDSHeaderDXT10) == 20);

	enum class BCnFmt
	{
		BC4,
		BC7_LINEAR,
		BC7_SRGB,
	};

	using Block4x4 = std::array<uint8_t, 64>;

	inline void GatherBlock(const uint8_t* pixels, int width, int height, int channels, int blockX, int blockY, Block4x4& out)
	{
		for (int py = 0; py < 4; ++py)
		{
			for (int px = 0; px < 4; ++px)
			{
				const int sx = std::min(blockX * 4 + px, width - 1);
				const int sy = std::min(blockY * 4 + py, height - 1);
				const uint8_t* src = pixels + static_cast<ptrdiff_t>((sy * width + sx) * channels);
				uint8_t* dst = out.data() + static_cast<ptrdiff_t>((py * 4 + px) * 4);
				dst[0] = channels > 0 ? src[0] : 0;
				dst[1] = channels > 1 ? src[1] : 0;
				dst[2] = channels > 2 ? src[2] : 0;
				dst[3] = channels > 3 ? src[3] : 255;
			}
		}
	}

	inline BCnFmt ChooseFormat(const std::filesystem::path& path, int channels)
	{
		if (channels == 1)
		{
			return BCnFmt::BC4;
		}

		const std::string stem = path.stem().string();
		std::string lower;
		lower.reserve(stem.size());
		for (const char c: stem)
		{
			lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		constexpr std::string_view kLinearKeywords[] = {
		        "normal",
		        "nrm",
		        "nrml",
		        "roughness",
		        "rough",
		        "metallic",
		        "metal",
		        "occlusion",
		        "ao",
		        "displacement",
		        "height",
		        "mask",
		};
		for (const std::string_view kw: kLinearKeywords)
		{
			if (lower.contains(kw))
			{
				return BCnFmt::BC7_LINEAR;
			}
		}

		return BCnFmt::BC7_SRGB;
	}

	inline void DownsampleBox2x2(const uint8_t* src, int srcW, int srcH, int channels, uint8_t* dst)
	{
		const int dstW = std::max(1, srcW / 2);
		const int dstH = std::max(1, srcH / 2);
		for (int y = 0; y < dstH; ++y)
		{
			for (int x = 0; x < dstW; ++x)
			{
				for (int c = 0; c < channels; ++c)
				{
					int sum = 0;
					int count = 0;
					for (int dy = 0; dy < 2; ++dy)
					{
						for (int dx = 0; dx < 2; ++dx)
						{
							const int sx = x * 2 + dx;
							const int sy = y * 2 + dy;
							if (sx < srcW && sy < srcH)
							{
								sum += src[(sy * srcW + sx) * channels + c];
								++count;
							}
						}
					}
					dst[(y * dstW + x) * channels + c] = static_cast<uint8_t>(sum / count);
				}
			}
		}
	}
} // namespace aether::assetpipeline
