#include "TextureProcessor.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// stb_image - decode PNG/JPG/TGA/BMP into raw RGBA pixels.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// bc7enc.h + bc7enc.cpp - BC7 block encoder.
// rgbcx.h + rgbcx.cpp - BC1/BC4/BC5 block encoders (compiled as separate TU).
#include <bc7enc.h>
#include <rgbcx.h>

namespace TextureProcessor
{
	namespace
	{
		// -------------------------------------------------------------------------
		// DDS format constants (DirectX definitions, platform-independent)
		// -------------------------------------------------------------------------

		constexpr uint32_t DDS_MAGIC = 0x20534444u; // "DDS "

		constexpr uint32_t DDSD_CAPS = 0x00000001u;
		constexpr uint32_t DDSD_HEIGHT = 0x00000002u;
		constexpr uint32_t DDSD_WIDTH = 0x00000004u;
		constexpr uint32_t DDSD_PIXELFORMAT = 0x00001000u;
		constexpr uint32_t DDSD_LINEARSIZE = 0x00080000u;

		constexpr uint32_t DDPF_FOURCC = 0x00000004u;

		constexpr uint32_t DDSCAPS_TEXTURE = 0x00001000u;
		constexpr uint32_t DDSCAPS_COMPLEX = 0x00000008u;
		constexpr uint32_t DDSCAPS_MIPMAP  = 0x00400000u;

		constexpr uint32_t FOURCC_DX10 = 0x30315844u; // "DX10"

		// DXGI formats for BCn
		constexpr uint32_t DXGI_FORMAT_BC4_UNORM      = 80u;
		constexpr uint32_t DXGI_FORMAT_BC7_UNORM       = 98u; // linear: normals, roughness, AO, metallic
		constexpr uint32_t DXGI_FORMAT_BC7_UNORM_SRGB  = 99u; // sRGB:   colour/albedo textures

		constexpr uint32_t D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3u;

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

		// -------------------------------------------------------------------------
		// Format selection
		// -------------------------------------------------------------------------

		enum class BCnFmt
		{
			BC4,        // single-channel linear
			BC7_LINEAR, // multi-channel linear (normal maps, roughness, AO, metallic…)
			BC7_SRGB,   // multi-channel sRGB   (albedo / colour textures)
		};

		BCnFmt ChooseFormat(const std::filesystem::path& path, int channels)
		{
			if (channels == 1)
				return BCnFmt::BC4;

			// Detect linear-data textures by filename keywords.
			// Normal maps, roughness, AO, metallic maps are linear - must NOT use sRGB.
			// BC5 is intentionally avoided: the engine shader reads .xyz and expects Z
			// in the B channel; BC5 only stores two channels, giving Z = -1 after remap.
			const std::string stem = path.stem().string();
			std::string lower;
			lower.reserve(stem.size());
			for (char c : stem)
				lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

			constexpr std::string_view kLinearKeywords[] = {
				"normal", "nrm", "nrml", "roughness", "rough", "metallic", "metal",
				"occlusion", "ao", "displacement", "height", "mask",
			};
			for (std::string_view kw : kLinearKeywords)
			{
				if (lower.find(kw) != std::string::npos)
					return BCnFmt::BC7_LINEAR;
			}

			return BCnFmt::BC7_SRGB;
		}

		// -------------------------------------------------------------------------
		// Block compression
		// -------------------------------------------------------------------------

		// Gather a 4x4 pixel block from the decoded image into a flat RGBA8 array.
		// Clamps to image edges so partial border blocks are handled correctly.
		void GatherBlock(const uint8_t* pixels, int width, int height, int channels, int blockX, int blockY,
		        uint8_t out[4 * 4 * 4]) // always RGBA8
		{
			for (int py = 0; py < 4; ++py)
			{
				for (int px = 0; px < 4; ++px)
				{
					const int sx = std::min(blockX * 4 + px, width - 1);
					const int sy = std::min(blockY * 4 + py, height - 1);
					const uint8_t* src = pixels + (sy * width + sx) * channels;
					uint8_t* dst = out + (py * 4 + px) * 4;
					dst[0] = channels > 0 ? src[0] : 0;
					dst[1] = channels > 1 ? src[1] : 0;
					dst[2] = channels > 2 ? src[2] : 0;
					dst[3] = channels > 3 ? src[3] : 255;
				}
			}
		}

		// Box-filter 2×2 downsample for mip chain generation.
		void DownsampleBox2x2(const uint8_t* src, int srcW, int srcH, int channels, uint8_t* dst)
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

		std::vector<std::byte> CompressBlocks(const uint8_t* pixels, int width, int height, int channels, BCnFmt fmt, const bc7enc_compress_block_params& bc7Params)
		{
			const int blockW = (width + 3) / 4;
			const int blockH = (height + 3) / 4;

			const std::size_t bytesPerBlock = (fmt == BCnFmt::BC4) ? 8u : 16u;
			std::vector<std::byte> out(static_cast<std::size_t>(blockW * blockH) * bytesPerBlock);
			std::byte* dst = out.data();

			uint8_t block[4 * 4 * 4];

			for (int by = 0; by < blockH; ++by)
			{
				for (int bx = 0; bx < blockW; ++bx)
				{
					GatherBlock(pixels, width, height, channels, bx, by, block);

					switch (fmt)
					{
						case BCnFmt::BC7_LINEAR:
						case BCnFmt::BC7_SRGB:
							bc7enc_compress_block(dst, block, &bc7Params);
							break;
						case BCnFmt::BC4:
							rgbcx::encode_bc4(dst, block, 4); // stride 4 -> red channel of RGBA
							break;
					}
					dst += bytesPerBlock;
				}
			}
			return out;
		}

		// -------------------------------------------------------------------------
		// DDS output
		// -------------------------------------------------------------------------

		struct MipData
		{
			int w, h;
			std::vector<std::byte> compressed;
		};

		std::vector<std::byte> BuildDDS(int baseWidth, int baseHeight, BCnFmt fmt, const std::vector<MipData>& mips)
		{
			uint32_t dxgiFmt = 0;
			switch (fmt)
			{
				case BCnFmt::BC4:
					dxgiFmt = DXGI_FORMAT_BC4_UNORM;
					break;
				case BCnFmt::BC7_LINEAR:
					dxgiFmt = DXGI_FORMAT_BC7_UNORM;
					break;
				case BCnFmt::BC7_SRGB:
					dxgiFmt = DXGI_FORMAT_BC7_UNORM_SRGB;
					break;
			}

			DDSHeader header;
			DDSHeaderDXT10 dx10{};

			header.dwHeight = static_cast<uint32_t>(baseHeight);
			header.dwWidth = static_cast<uint32_t>(baseWidth);
			header.dwPitchOrLinearSize = static_cast<uint32_t>(mips.empty() ? 0u : mips[0].compressed.size());
			header.dwMipMapCount = static_cast<uint32_t>(mips.size());
			header.dwCaps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
			dx10.dxgiFormat = dxgiFmt;

			std::size_t totalPixelData = 0;
			for (const auto& m : mips)
				totalPixelData += m.compressed.size();

			const std::size_t totalSize = sizeof(uint32_t) + sizeof(DDSHeader) + sizeof(DDSHeaderDXT10) + totalPixelData;

			std::vector<std::byte> dds(totalSize);
			std::byte* p = dds.data();

			auto write = [&](const void* src, std::size_t n)
			{
				std::memcpy(p, src, n);
				p += n;
			};

			write(&DDS_MAGIC, sizeof(DDS_MAGIC));
			write(&header, sizeof(header));
			write(&dx10, sizeof(dx10));
			for (const auto& m : mips)
				write(m.compressed.data(), m.compressed.size());

			return dds;
		}
		struct StbiDeleter { void operator()(uint8_t* p) { stbi_image_free(p); } };
		using StbiImage = std::unique_ptr<uint8_t, StbiDeleter>;
	} // namespace

	// -------------------------------------------------------------------------

	// One-time initialisation for bc7enc and rgbcx, guarded by call_once.
	static void InitEncoders()
	{
		static std::once_flag s_flag;
		std::call_once(s_flag, [] {
			bc7enc_compress_block_init();
			rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
		});
	}

	std::vector<std::byte> ToDDS(const std::vector<std::byte>& imageData, const std::filesystem::path& sourcePath, BC7Quality quality)
	{
		InitEncoders();

		int width, height, srcChannels;
		StbiImage pixels(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(imageData.data()), static_cast<int>(imageData.size()), &width, &height, &srcChannels, 0));

		if (!pixels)
		{
			std::cerr << "  TextureProcessor: stb_image failed for " << sourcePath << ": " << stbi_failure_reason() << "\n";
			return {};
		}

		const BCnFmt fmt = ChooseFormat(sourcePath, srcChannels);

		bc7enc_compress_block_params bc7Params;
		bc7enc_compress_block_params_init(&bc7Params);
		switch (quality)
		{
			case BC7Quality::Ultra:
				bc7Params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
				break;
			case BC7Quality::High:
				bc7Params.m_uber_level = 2;
				break;
			default:
				break;
		}

		// Build mip chain
		std::vector<MipData> mips;
		int mipW = width, mipH = height;
		const uint8_t* srcPixels = pixels.get();
		std::vector<uint8_t> mipStorage;

		while (true)
		{
			auto blocks = CompressBlocks(srcPixels, mipW, mipH, srcChannels, fmt, bc7Params);
			mips.push_back({ mipW, mipH, std::move(blocks) });

			if (mipW == 1 && mipH == 1)
				break;

			const int newW = std::max(1, mipW / 2);
			const int newH = std::max(1, mipH / 2);
			mipStorage.resize(static_cast<std::size_t>(newW * newH) * srcChannels);
			DownsampleBox2x2(srcPixels, mipW, mipH, srcChannels, mipStorage.data());

			mipW = newW;
			mipH = newH;
			srcPixels = mipStorage.data();
		}

		return BuildDDS(width, height, fmt, mips);
	}
} // namespace TextureProcessor
