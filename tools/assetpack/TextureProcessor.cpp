#include "TextureProcessor.hpp"

#include "DDSFormat.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <stb_image.h>

#include <bc7enc.h>
#include <rgbcx.h>

namespace aether::assetpipeline
{
	namespace TextureProcessor
	{
		namespace
		{
			std::vector<std::byte> CompressBlocks(const uint8_t* pixels, int width, int height, int channels, BCnFmt fmt, const bc7enc_compress_block_params& bc7Params)
			{
				const int blockW = (width + 3) / 4;
				const int blockH = (height + 3) / 4;

				const std::size_t bytesPerBlock = (fmt == BCnFmt::BC4) ? 8u : 16u;
				std::vector<std::byte> out(static_cast<std::size_t>(blockW * blockH) * bytesPerBlock);
				std::byte* dst = out.data();

				Block4x4 block{};

				for (int by = 0; by < blockH; ++by)
				{
					for (int bx = 0; bx < blockW; ++bx)
					{
						GatherBlock(pixels, width, height, channels, bx, by, block);

						switch (fmt)
						{
							case BCnFmt::BC7_LINEAR:
							case BCnFmt::BC7_SRGB:
								bc7enc_compress_block(dst, block.data(), &bc7Params);
								break;
							case BCnFmt::BC4:
								rgbcx::encode_bc4(dst, block.data(), 4);
								break;
						}
						dst += bytesPerBlock;
					}
				}
				return out;
			}

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
				for (const auto& m: mips)
				{
					totalPixelData += m.compressed.size();
				}

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
				for (const auto& m: mips)
				{
					write(m.compressed.data(), m.compressed.size());
				}

				return dds;
			}

			struct StbiDeleter
			{
				void operator()(uint8_t* p)
				{
					stbi_image_free(p);
				}
			};

			using StbiImage = std::unique_ptr<uint8_t, StbiDeleter>;
		} // namespace

		static void InitEncoders()
		{
			static std::once_flag s_flag;
			std::call_once(s_flag,
			        []
			        {
				        bc7enc_compress_block_init();
				        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
			        });
		}

		ByteBuffer ToDDS(std::span<const std::byte> imageData, const std::filesystem::path& sourcePath, BC7Quality quality)
		{
			InitEncoders();

			int width = 0, height = 0, srcChannels = 0;
			const StbiImage pixels(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(imageData.data()), static_cast<int>(imageData.size()), &width, &height, &srcChannels, 0));

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

			std::vector<MipData> mips;
			int mipW = width, mipH = height;
			const uint8_t* srcPixels = pixels.get();
			std::vector<uint8_t> mipStorage;

			while (true)
			{
				auto blocks = CompressBlocks(srcPixels, mipW, mipH, srcChannels, fmt, bc7Params);
				mips.push_back({mipW, mipH, std::move(blocks)});

				if (mipW == 1 && mipH == 1)
				{
					break;
				}

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
} // namespace aether::assetpipeline
