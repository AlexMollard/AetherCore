#include "editor/EditorProjectPublisher.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#	undef CopyFile
#endif

#include "AssetPipeline.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ShaderCompiler.hpp"
#include "editor/publish/PublishSteps.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "scene/SceneWorkflow.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kProjectFileName = "ProjectSettings.toml";

		std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.lexically_normal().generic_string();
		}

		bool HasProjectDescriptor(const std::filesystem::path& root)
		{
			return io::file_util::Exists(root / kProjectFileName);
		}
	} // namespace

	PublishPlan PlanPublish(const app::EditorProjectContext& project)
	{
		return MakePublishPlan(project, CurrentPublishEnvironment());
	}

	EditorProjectActionResult PackProject(const app::EditorProjectContext& project)
	{
		if (!project.IsLoaded() || !HasProjectDescriptor(project.root))
		{
			return {.succeeded = false, .message = "No project is open.", .remediation = "Open a project from the launcher first."};
		}

		const std::filesystem::path outputDir = project.root / "Builds" / "Pack";
		if (auto dirResult = io::file_util::CreateDirectories(outputDir); !dirResult)
		{
			return {.succeeded = false, .message = "Could not create the pack output folder: " + dirResult.error().message};
		}

		// Compile the project's Slang shaders first so their .spv land in the pak.
		std::filesystem::path shaderSpirvDir;
		if (CanCompileShaders())
		{
			const std::filesystem::path intermediateShaderDir = ProjectShaderIntermediateDir(project.root);
			std::error_code ec;
			std::filesystem::remove_all(intermediateShaderDir, ec);
			if (ec)
			{
				return {.succeeded = false, .message = "Could not clean shader intermediates: " + ec.message()};
			}
			const ShaderCompileResult shaderResult = CompileProject(project.root);
			if (!shaderResult.ok)
			{
				return {.succeeded = false, .message = "Project shader compile failed: " + shaderResult.message, .outputPath = outputDir / "project.pak"};
			}
			shaderSpirvDir = intermediateShaderDir;
		}

		app::scene::CookProjectBinaries();

		const std::filesystem::path outputPak = outputDir / "project.pak";
		const assetpipeline::PackResult packResult = assetpipeline::PackProject(project.root, outputPak, {.importMaterials = true, .projectLayout = true, .shaderSpirvDir = shaderSpirvDir});
		if (!packResult.ok)
		{
			return {.succeeded = false, .message = packResult.message, .outputPath = outputPak};
		}

		// Refresh the editor's own copy so the dev GameRuntime picks the new pak up. This is
		// Pack's job specifically - publish never writes outside its own output folder.
		const std::filesystem::path exeDataDir = io::PlatformPaths::GetExecutableDir() / "data";
		if (outputDir != exeDataDir)
		{
			if (auto dirResult = io::file_util::CreateDirectories(exeDataDir); dirResult)
			{
				if (auto result = io::file_util::CopyFile(outputPak, exeDataDir / "project.pak"); !result)
				{
					AE_WARN(LogCategory::App, "Failed to copy project.pak: {}", result.error().message);
				}
			}
		}

		std::uintmax_t size = 0;
		if (auto fileSizeResult = io::file_util::FileSize(outputPak))
		{
			size = *fileSizeResult;
		}
		AE_INFO(LogCategory::App, "Packed project '{}' to {}", project.name, DisplayPath(outputPak));
		return {.succeeded = true, .message = "Packed project.pak (" + std::to_string(size / 1024) + " KB).", .outputPath = outputPak};
	}

	EditorProjectActionResult PublishProject(const app::EditorProjectContext& project, const EditorProjectPublishProgress& progress)
	{
		const PublishPlan plan = PlanPublish(project);
		const PublishToolchain toolchain = MakePublishToolchain();
		PublishContext context;

		const std::span<const PublishStep> steps = PublishStepList();
		for (std::size_t i = 0; i < steps.size(); ++i)
		{
			if (progress)
			{
				progress(static_cast<float>(i) / static_cast<float>(steps.size()), steps[i].name);
			}
			const StepResult result = steps[i].run(plan, context, toolchain);
			if (!result.ok)
			{
				AE_ERROR(LogCategory::App, "Publish failed at '{}': {}", steps[i].name, result.message);
				return {.succeeded = false, .message = result.message, .remediation = result.remediation, .outputPath = plan.outputDir};
			}
		}

		if (progress)
		{
			progress(1.0f, "Published");
		}
		AE_INFO(LogCategory::App, "Published project '{}' to {}", project.name, DisplayPath(plan.outputDir));
		return {.succeeded = true, .message = context.reportSummary, .outputPath = plan.outputDir};
	}
} // namespace aether::editor
