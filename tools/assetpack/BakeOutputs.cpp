#include "BakeOutputs.hpp"

#include <cstring>
#include <fstream>

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	namespace
	{
		bool WriteBytes(const fs::path& path, const ByteBuffer& data, std::string& error)
		{
			std::error_code ec;
			fs::create_directories(path.parent_path(), ec);
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				error = "cannot open '" + path.generic_string() + "' for write";
				return false;
			}
			out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
			if (!out)
			{
				error = "failed writing '" + path.generic_string() + "'";
				return false;
			}
			return true;
		}
	} // namespace

	BakeWriteResult WriteBakedOutputs(const MeshProcessor::ProcessedResult& baked, const fs::path& modelDiskPath, const fs::path& projectRoot)
	{
		BakeWriteResult result;

		const std::string stem = modelDiskPath.stem().generic_string();
		const fs::path modelDir = modelDiskPath.parent_path();
		result.meshPath = modelDir / (stem + ".mesh");

		std::vector<fs::path> written;
		auto writeTracked = [&](const fs::path& outPath, const ByteBuffer& data) -> bool
		{
			std::string writeError;
			if (!WriteBytes(outPath, data, writeError))
			{
				for (const fs::path& p: written)
				{
					std::error_code rmEc;
					fs::remove(p, rmEc);
				}
				result.error = writeError;
				return false;
			}
			written.push_back(outPath);
			return true;
		};

		if (!writeTracked(result.meshPath, baked.meshData))
		{
			return result;
		}
		if (!baked.skelData.empty() && !writeTracked(modelDir / (stem + ".skel"), baked.skelData))
		{
			return result;
		}
		if (!baked.animsetData.empty() && !writeTracked(modelDir / (stem + ".animset"), baked.animsetData))
		{
			return result;
		}
		for (const auto& [fileName, animData]: baked.animFiles)
		{
			if (!animData.empty() && !writeTracked(projectRoot / "animations" / fileName, animData))
			{
				return result;
			}
		}
		for (const auto& [matRelPath, matData]: baked.materialFiles)
		{
			if (matData.empty())
			{
				continue;
			}
			const fs::path matOutPath = projectRoot / matRelPath;
			std::ifstream existing(matOutPath, std::ios::binary);
			if (existing.good())
			{
				const std::vector<char> existingBytes((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
				if (existingBytes.size() != matData.size() || std::memcmp(existingBytes.data(), matData.data(), matData.size()) != 0)
				{
					result.warnings.push_back("generates a material at '" + matRelPath + "' that already exists with DIFFERENT content - two models share a glTF material name but not its data; whichever bakes last wins");
				}
			}
			if (!writeTracked(matOutPath, matData))
			{
				return result;
			}
		}

		result.ok = true;
		return result;
	}
} // namespace aether::assetpipeline
