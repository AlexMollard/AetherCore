#pragma once
#include <cstddef>
#include <filesystem>
#include <vector>

namespace TextureProcessor
{
	// Decode a PNG/JPG/TGA image and re-encode it as a BCn DDS file.
	//
	// Format selection per source filename:
	//   "normal" / "nrm" in stem  -> BC7_LINEAR (RGBA, engine reads .xyz)
	//   single-channel image       -> BC4
	//   everything else            -> BC7_SRGB (highest quality, handles RGB + RGBA)
	//
	// Returns empty if the input is not a supported image or encoding fails.
	// Output extension for the virtual path is always ".texture".
	std::vector<std::byte> ToDDS(
	    const std::vector<std::byte>& imageData,
	    const std::filesystem::path&  sourcePath);
} // namespace TextureProcessor
