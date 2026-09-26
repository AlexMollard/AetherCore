#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Shared by the runtime texture upload (Texture.cpp) and the offline bake (AssetPacker's
// DDSFormat.hpp), so a loose PNG and a baked .texture filter the same way.
namespace aether
{
	// Fully transparent texels (alpha 0) take the colour of their nearest visible neighbours.
	// PS2 cut-out art stores near-black RGB there (grass fringe cards: (6, 27, 14) against
	// (40, 61, 22) opaque), and an alpha-weighted downsample leaves an all-transparent block
	// pure black. Alpha is untouched, so nothing new shows - but bilinear filtering at a cut-out
	// edge blends in the texels on both sides, so every card read with a dark outline that
	// traced its quad on the ground. RGBA only; flood fills up to kMaxPasses texels out, then
	// the mean visible colour.
	inline void DilateTransparentRgb(uint8_t* px, int w, int h, int channels)
	{
		if (channels != 4)
		{
			return;
		}
		constexpr int kMaxPasses = 8;
		const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
		std::vector<uint8_t> filled(n);
		long long mean[3] = {0, 0, 0};
		std::size_t visible = 0;
		for (std::size_t i = 0; i < n; ++i)
		{
			filled[i] = px[i * 4 + 3] != 0 ? 1 : 0;
			if (filled[i] != 0)
			{
				for (int c = 0; c < 3; ++c)
				{
					mean[c] += px[i * 4 + c];
				}
				++visible;
			}
		}
		if (visible == 0 || visible == n)
		{
			return;
		}
		std::vector<std::size_t> frontier;
		for (int pass = 0; pass < kMaxPasses; ++pass)
		{
			frontier.clear();
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (filled[i] != 0)
					{
						continue;
					}
					int sum[3] = {0, 0, 0};
					int count = 0;
					for (int dy = -1; dy <= 1; ++dy)
					{
						for (int dx = -1; dx <= 1; ++dx)
						{
							const int sx = x + dx;
							const int sy = y + dy;
							if ((dx == 0 && dy == 0) || sx < 0 || sy < 0 || sx >= w || sy >= h)
							{
								continue;
							}
							const std::size_t j = static_cast<std::size_t>(sy) * w + sx;
							if (filled[j] == 1)
							{
								for (int c = 0; c < 3; ++c)
								{
									sum[c] += px[j * 4 + c];
								}
								++count;
							}
						}
					}
					if (count > 0)
					{
						for (int c = 0; c < 3; ++c)
						{
							px[i * 4 + c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
						}
						frontier.push_back(i);
					}
				}
			}
			if (frontier.empty())
			{
				break;
			}
			// Marked only after the whole pass, so one pass grows the fill by exactly one texel.
			for (const std::size_t i: frontier)
			{
				filled[i] = 1;
			}
		}
		for (std::size_t i = 0; i < n; ++i)
		{
			if (filled[i] == 0)
			{
				for (int c = 0; c < 3; ++c)
				{
					px[i * 4 + c] = static_cast<uint8_t>(mean[c] / static_cast<long long>(visible));
				}
			}
		}
	}
} // namespace aether
