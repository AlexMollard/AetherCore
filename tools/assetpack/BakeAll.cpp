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

		// The newest mtime among `modelPath` itself and every EXTERNAL buffer/image it
		// references - editing a `.gltf`'s external `.bin`/texture must be exactly as
		// stale-triggering as editing the model file itself (a grafted animation clip or a
		// root-motion fix living only in an external `.bin` is otherwise invisible until
		// someone deletes the stale bake by hand). A missing dependency is ignored here
		// (best-effort freshness signal only); MeshProcessor::Process reports it properly
		// if the model is actually (re)baked.
		std::filesystem::file_time_type SourceFreshness(const fs::path& modelPath)
		{
			std::error_code ec;
			auto newest = fs::last_write_time(modelPath, ec);
			for (const fs::path& dep: MeshProcessor::CollectExternalSourceFiles(modelPath))
			{
				std::error_code depEc;
				const auto depTime = fs::last_write_time(dep, depEc);
				if (!depEc && depTime > newest)
				{
					newest = depTime;
				}
			}
			return newest;
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

			const fs::path meshPath = modelPath.parent_path() / (modelPath.stem().string() + ".mesh");
			const auto sourceTime = SourceFreshness(modelPath);
			std::error_code meshEc;
			const bool meshExisted = fs::exists(meshPath, meshEc);
			bool stale = true;
			if (meshExisted)
			{
				const auto meshTime = fs::last_write_time(meshPath, meshEc);
				stale = static_cast<bool>(meshEc) || sourceTime > meshTime;
			}
			if (!stale)
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
			if (processed.meshData.empty())
			{
				// Ambiguous by design (see MeshProcessor::Process): a legitimately
				// meshless source (an animation-only glTF) and a corrupt one both land
				// here with no way to tell them apart. Non-fatal - failing the whole
				// batch over an asset that was never going to contribute a mesh would be
				// wrong; a genuinely broken file still surfaces later as the runtime's
				// missing-mesh error.
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
