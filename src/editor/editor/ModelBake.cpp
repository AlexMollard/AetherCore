#include "editor/ModelBake.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "MeshProcessor.hpp"

#include "assets/GltfAsset.hpp"
#include "editor/EditorProjectContext.hpp"
#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace fs = std::filesystem;

	namespace
	{
		std::string StripMount(const std::string& vfs)
		{
			const auto ss = vfs.find("://");
			return ss == std::string::npos ? vfs : vfs.substr(ss + 3);
		}

		bool WriteBaked(const fs::path& outPath, const assetpipeline::ByteBuffer& data, std::string& error)
		{
			std::error_code ec;
			fs::create_directories(outPath.parent_path(), ec);
			std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				error = "cannot open '" + outPath.generic_string() + "' for write";
				return false;
			}
			out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
			if (!out)
			{
				error = "failed writing '" + outPath.generic_string() + "'";
				return false;
			}
			return true;
		}
	} // namespace

	bool EnsureModelBaked(const std::string& vfsModelPath, const app::EditorProjectContext& project, std::string& error)
	{
		const std::string meshVfs = assets::GltfAsset::ResolveMeshPath(vfsModelPath);
		if (!meshVfs.empty() && io::FileSystem::Exists(meshVfs))
		{
			return true;
		}

		if (!project.IsLoaded())
		{
			error = "no project loaded";
			return false;
		}
		return EnsureModelBaked(vfsModelPath, project.root, error);
	}

	bool EnsureModelBaked(const std::string& vfsModelPath, const fs::path& projectRoot, std::string& error)
	{
		const std::string meshVfs = assets::GltfAsset::ResolveMeshPath(vfsModelPath);
		if (!meshVfs.empty() && io::FileSystem::Exists(meshVfs))
		{
			return true;
		}

		const auto raw = io::FileSystem::ReadFile(vfsModelPath);
		if (!raw)
		{
			error = "cannot read model '" + vfsModelPath + "'";
			return false;
		}

		const std::string rel = StripMount(vfsModelPath);
		const fs::path diskPath = projectRoot / rel;

		const auto result = assetpipeline::MeshProcessor::Process(std::span<const std::byte>(raw->data(), raw->size()), diskPath, rel, projectRoot);

		if (result.meshData.empty())
		{
			error = "no mesh geometry produced from '" + vfsModelPath + "' (animation-only or unsupported glTF)";
			return false;
		}

		// Mirror AssetProcessor's pak layout, but under the project root on disk.
		const std::string stem = fs::path(rel).stem().generic_string();
		const fs::path modelDir = (projectRoot / rel).parent_path();

		// EnsureModelBaked's OWN "already baked" check above only looks for the
		// .mesh file - so a bake that fails partway (mesh written, then an
		// animation or material write fails) must never leave that .mesh sitting
		// on disk: every future call would see it, decide this model is already
		// fully baked, and silently skip re-baking forever, serving whatever
		// incomplete/stale set of skel/animset/anim/material files this failed
		// attempt happened to write before the error. Every successful write goes
		// into `written`; any failure erases everything this call wrote (only
		// this call's own files - nothing that predates it) before returning, so
		// the mesh-existence check is honest again on the next attempt instead of
		// a landmine buried by whatever failed today.
		std::vector<fs::path> written;
		auto writeTracked = [&](const fs::path& outPath, const assetpipeline::ByteBuffer& data) -> bool
		{
			if (!WriteBaked(outPath, data, error))
			{
				for (const fs::path& p: written)
				{
					std::error_code ec;
					fs::remove(p, ec);
				}
				return false;
			}
			written.push_back(outPath);
			return true;
		};

		if (!writeTracked(modelDir / (stem + ".mesh"), result.meshData))
		{
			return false;
		}
		if (!result.skelData.empty() && !writeTracked(modelDir / (stem + ".skel"), result.skelData))
		{
			return false;
		}
		if (!result.animsetData.empty() && !writeTracked(modelDir / (stem + ".animset"), result.animsetData))
		{
			return false;
		}
		for (const auto& [fileName, animData]: result.animFiles)
		{
			if (!animData.empty() && !writeTracked(projectRoot / "animations" / fileName, animData))
			{
				return false;
			}
		}
		for (const auto& [matVfsPath, matData]: result.materialFiles)
		{
			if (matData.empty())
			{
				continue;
			}
			const fs::path matOutPath = projectRoot / matVfsPath;
			// The auto-generated material's on-disk path is derived purely from its glTF
			// material NAME plus the model's directory (see MeshProcessor's
			// GenerateGlTFMaterialData) - intentional when two models share a name AND
			// content (Kenney-style: many props reusing one "colormap" material/atlas),
			// but a silent, wrong-looking collision when two DIFFERENT models happen to
			// reuse the same name with different content (e.g. a batch generator that
			// names every material "IconMaterial"): whichever gets baked last wins, and
			// every earlier one quietly renders with the wrong colour forever. Detect a
			// content mismatch and say so loudly instead of guessing which model is right.
			std::ifstream existing(matOutPath, std::ios::binary);
			if (existing.good())
			{
				const std::vector<char> existingBytes((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
				if (existingBytes.size() != matData.size() || std::memcmp(existingBytes.data(), matData.data(), matData.size()) != 0)
				{
					AE_WARN(LogCategory::App,
					        "Model '{}' generates a material at '{}' that already exists with DIFFERENT content - two models in this directory share a glTF material name but not its data, so whichever imports last silently wins and the other(s) render with the wrong colour. Give each model's material a unique name.",
					        vfsModelPath,
					        matVfsPath);
				}
			}
			if (!writeTracked(matOutPath, matData))
			{
				return false;
			}
		}

		AE_INFO(LogCategory::App, "Imported model '{}' ({} material(s), {} clip(s)) -> {}.mesh", vfsModelPath, result.materialFiles.size(), result.animFiles.size(), stem);
		return true;
	}
} // namespace aether::editor
