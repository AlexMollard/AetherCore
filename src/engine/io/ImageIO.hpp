#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace aether::io
{
	// Write an RGBA8 buffer (width*height*4 bytes, row-major, byte 0 = R) to a PNG.
	// Creates parent directories. Thin wrapper over stb_image_write so callers
	// outside the engine's stb include scope can do PNG I/O.
	[[nodiscard]] bool WritePngRgba(const std::filesystem::path& path, const std::uint8_t* rgba, int width, int height);

	// Load a PNG as RGBA8 into outRgba (width*height*4 bytes). Returns false on
	// failure (missing/corrupt file).
	[[nodiscard]] bool LoadPngRgba(const std::filesystem::path& path, std::vector<std::uint8_t>& outRgba, int& width, int& height);
} // namespace aether::io
