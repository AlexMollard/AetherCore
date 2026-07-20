#include "io/ImageIO.hpp"

#include <cstring>
#include <system_error>

#include <stb_image.h>
#include <stb_image_write.h>

namespace aether::io
{
	bool WritePngRgba(const std::filesystem::path& path, const std::uint8_t* rgba, int width, int height)
	{
		if (rgba == nullptr || width <= 0 || height <= 0)
		{
			return false;
		}
		std::error_code ec;
		if (path.has_parent_path())
		{
			std::filesystem::create_directories(path.parent_path(), ec);
		}
		return stbi_write_png(path.string().c_str(), width, height, 4, rgba, width * 4) != 0;
	}

	bool LoadPngRgba(const std::filesystem::path& path, std::vector<std::uint8_t>& outRgba, int& width, int& height)
	{
		int w = 0;
		int h = 0;
		int channels = 0;
		stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
		if (data == nullptr || w <= 0 || h <= 0)
		{
			if (data != nullptr)
			{
				stbi_image_free(data);
			}
			return false;
		}
		outRgba.resize(static_cast<std::size_t>(w) * h * 4);
		std::memcpy(outRgba.data(), data, outRgba.size());
		stbi_image_free(data);
		width = w;
		height = h;
		return true;
	}
} // namespace aether::io
