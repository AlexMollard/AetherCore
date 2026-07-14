#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

#include "PipelineUtils.hpp"

namespace aether::assetpipeline
{
	namespace MeshProcessor
	{
		struct ProcessedResult
		{
			ByteBuffer skelData;
			ByteBuffer meshData;
			ByteBuffer animsetData;
			std::vector<std::pair<std::string, ByteBuffer>> animFiles;
			std::vector<std::pair<std::string, ByteBuffer>> materialFiles;
			std::string skeletonHash;
		};

		[[nodiscard]] ProcessedResult Process(std::span<const std::byte> gltfData, const std::filesystem::path& sourcePath, const std::string& virtualPath, const std::filesystem::path& sourceDir);
	} // namespace MeshProcessor
} // namespace aether::assetpipeline
