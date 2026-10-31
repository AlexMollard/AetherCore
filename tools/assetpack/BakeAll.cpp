#include "BakeAll.hpp"

#include "BakeOutputs.hpp"
#include "MeshProcessor.hpp"
#include "PipelineUtils.hpp"

#include <algorithm>
#include <cctype>
#include <span>
#include <system_error>

namespace aether::assetpipeline
{
	namespace
	{
		bool IsModelFile(const fs::path& p)
		{
			std::string ext = p.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return ext == ".glb" || ext == ".gltf";
		}
	} // namespace

	BakeAllResult BakeAllModels(const std::filesystem::path& projectRoot)
	{
		BakeAllResult result;
		const fs::path modelsDir = projectRoot / "assets" / "models";
		std::error_code dirEc;
		if (!fs::is_directory(modelsDir, dirEc))
		{
			return result; // no models directory: nothing to bake, not an error
		}

		std::error_code walkEc;
		for (auto it = fs::recursive_directory_iterator(modelsDir, walkEc); it != fs::recursive_directory_iterator(); it.increment(walkEc))
		{
			if (walkEc)
			{
				result.failures.push_back(walkEc.message());
				++result.failed;
				walkEc.clear();
				continue;
			}

			std::error_code fileEc;
			if (!it->is_regular_file(fileEc) || fileEc || !IsModelFile(it->path()))
			{
				continue;
			}
			const fs::path modelPath = it->path();
			const std::string relStr = fs::relative(modelPath, projectRoot).generic_string();

			if (HasCurrentModelBake(modelPath, projectRoot))
			{
				++result.upToDate;
				continue;
			}

			ByteBuffer raw;
			if (!ReadWholeFile(modelPath, raw))
			{
				result.failures.push_back(relStr + ": cannot read file");
				++result.failed;
				continue;
			}
			const auto processed = MeshProcessor::Process(std::span<const std::byte>(raw.data(), raw.size()), modelPath, relStr, projectRoot);
			if (!processed.error.empty())
			{
				result.failures.push_back(relStr + ": " + processed.error);
				++result.failed;
				continue;
			}
			if (processed.meshData.empty())
			{
				// Animation-only glTFs legitimately have no mesh geometry.
				result.skippedModels.push_back(relStr + ": no mesh geometry produced");
				++result.skipped;
				continue;
			}
			const BakeWriteResult write = WriteBakedOutputs(processed, modelPath, projectRoot);
			if (!write.ok)
			{
				result.failures.push_back(relStr + ": " + write.error);
				++result.failed;
				continue;
			}
			result.bakedModels.push_back(relStr);
			++result.baked;
		}

		return result;
	}
} // namespace aether::assetpipeline
