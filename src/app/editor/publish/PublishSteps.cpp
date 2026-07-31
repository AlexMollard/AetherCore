#include "editor/publish/PublishSteps.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>

#include "AssetPipeline.hpp"
#include "editor/ShaderCompiler.hpp"
#include "editor/publish/PublishPlan.hpp"
#include "editor/publish/PublishReport.hpp"
#include "editor/publish/PublishVerify.hpp"
#include "io/FileUtil.hpp"
#include "io/Process.hpp"
#include "project/ProjectStartupScene.hpp"
#include "scene/SceneWorkflow.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

// io/Process.hpp drags in Windows.h, whose CopyFile macro rewrites every call to
// io::file_util::CopyFile into CopyFileA. Same fix as ProjectCommon.cpp.
#ifdef CopyFile
#	undef CopyFile
#endif

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kEngineBugRemediation = "This is an engine bug - report it.";

		std::string LowerAscii(std::string value)
		{
			std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		StepResult Failed(std::string message, std::string remediation)
		{
			return {.ok = false, .message = std::move(message), .remediation = std::move(remediation)};
		}

		bool CopyOne(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error)
		{
			if (!io::file_util::Exists(from))
			{
				error = "Missing from the editor bundle: " + from.generic_string();
				return false;
			}
			if (auto dirResult = io::file_util::CreateDirectories(to.parent_path()); !dirResult)
			{
				error = dirResult.error().message;
				return false;
			}
			if (auto result = io::file_util::CopyFile(from, to); !result)
			{
				error = result.error().message;
				return false;
			}
			return true;
		}

		bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(from, ec))
			{
				error = "Missing from the editor bundle: " + from.generic_string();
				return false;
			}
			if (auto dirResult = io::file_util::CreateDirectories(to); !dirResult)
			{
				error = dirResult.error().message;
				return false;
			}
			for (const auto& entry: std::filesystem::recursive_directory_iterator(from, ec))
			{
				if (ec)
				{
					error = ec.message();
					return false;
				}
				const std::filesystem::path rel = std::filesystem::relative(entry.path(), from, ec);
				if (ec)
				{
					error = ec.message();
					return false;
				}
				if (entry.is_directory(ec))
				{
					if (auto dirResult = io::file_util::CreateDirectories(to / rel); !dirResult)
					{
						error = dirResult.error().message;
						return false;
					}
					continue;
				}
				if (!entry.is_regular_file(ec))
				{
					continue;
				}
				// Never carry build leftovers into the package; the prune step would only have
				// to delete them again, and a missed one fails verification.
				if (IsPrunablePublishedFile(entry.path()) || IsPakSidecarFile(entry.path()))
				{
					continue;
				}
				if (!CopyOne(entry.path(), to / rel, error))
				{
					return false;
				}
			}
			return true;
		}

		// ── Steps ──────────────────────────────────────────────────────────────────

		StepResult ValidateProject(const PublishPlan& plan, PublishContext&, const PublishToolchain&)
		{
			if (plan.projectRoot.empty() || !io::file_util::Exists(plan.projectFile))
			{
				return Failed("No project is open, or its ProjectSettings.toml is missing.", "Open a project from the launcher, then publish again.");
			}

			const std::string startupScene = app::ReadProjectStartupScene(plan.projectFile);
			std::string error;
			if (!app::ValidateProjectStartupScene(plan.scenesDir, startupScene, error))
			{
				return Failed(std::move(error), "Set a startup scene in the Project panel, or with the star in the Scenes list.");
			}
			return {};
		}

		StepResult CleanOutput(const PublishPlan& plan, PublishContext&, const PublishToolchain&)
		{
			std::error_code ec;
			std::filesystem::remove_all(plan.outputDir, ec);
			if (ec)
			{
				return Failed("Could not clear the output folder: " + ec.message(), "Close anything using " + plan.outputDir.generic_string() + " and publish again.");
			}
			if (auto dirResult = io::file_util::CreateDirectories(plan.outputDir); !dirResult)
			{
				return Failed("Could not create the output folder: " + dirResult.error().message, "Check write permissions on " + plan.outputDir.generic_string() + ".");
			}
			return {};
		}

		StepResult PackProjectAssets(const PublishPlan& plan, PublishContext& context, const PublishToolchain&)
		{
			std::filesystem::path shaderSpirvDir;
			if (CanCompileShaders())
			{
				const std::filesystem::path intermediateShaderDir = ProjectShaderIntermediateDir(plan.projectRoot);
				std::error_code ec;
				std::filesystem::remove_all(intermediateShaderDir, ec);
				if (ec)
				{
					return Failed("Could not clean shader intermediates: " + ec.message(), "Close anything using the project's Builds folder and publish again.");
				}
				const ShaderCompileResult shaderResult = CompileProject(plan.projectRoot);
				if (!shaderResult.ok)
				{
					return Failed("Project shader compile failed: " + shaderResult.message, "Fix the shader error above, then publish again.");
				}
				shaderSpirvDir = intermediateShaderDir;
			}

			// Cook fresh scene/prefab binaries so the pak ships the fast binary form.
			app::scene::CookProjectBinaries();

			const std::filesystem::path outputPak = plan.outputDir / "data" / "project.pak";
			if (auto dirResult = io::file_util::CreateDirectories(outputPak.parent_path()); !dirResult)
			{
				return Failed("Could not create the package data folder: " + dirResult.error().message, "Check write permissions on the Builds folder.");
			}
			const assetpipeline::PackResult packResult = assetpipeline::PackProject(plan.projectRoot, outputPak, {.importMaterials = true, .projectLayout = true, .shaderSpirvDir = shaderSpirvDir});
			if (!packResult.ok)
			{
				return Failed("Could not pack the project: " + packResult.message, "Check the project's assets for the file named above.");
			}
			context.packedProjectPak = outputPak;
			return {};
		}

		// One staging step with an explicit payload list. This replaces the two divergent
		// paths the old publisher had (a prebuilt package template, or a partial copy from
		// the executable directory), which shipped different payloads depending on a toggle.
		StepResult StageRuntime(const PublishPlan& plan, PublishContext&, const PublishToolchain&)
		{
			std::string error;
			if (!CopyOne(plan.bundleDir / plan.runtimeExeName, plan.outputDir / plan.runtimeExeName, error))
			{
				return Failed("Could not stage the game runtime: " + error, "Rebuild the editor - CMake builds the runtime alongside it.");
			}

			// Native dependencies sit beside the executable. Aftermath is dev-only and
			// editor-gated, so it is skipped rather than staged and pruned later.
			std::error_code ec;
			for (const auto& entry: std::filesystem::directory_iterator(plan.bundleDir, ec))
			{
				if (ec)
				{
					return Failed("Could not read the editor bundle: " + ec.message(), std::string(kEngineBugRemediation));
				}
				if (!entry.is_regular_file(ec))
				{
					continue;
				}
				const std::string ext = LowerAscii(entry.path().extension().generic_string());
				if (ext != ".dll" && ext != ".so" && ext != ".dylib")
				{
					continue;
				}
				if (IsAftermathRuntimeFile(entry.path()))
				{
					continue;
				}
				if (!CopyOne(entry.path(), plan.outputDir / entry.path().filename(), error))
				{
					return Failed("Could not stage a runtime library: " + error, std::string(kEngineBugRemediation));
				}
			}

			const std::filesystem::path bundleData = plan.bundleDir / "data";
			const std::filesystem::path packageData = plan.outputDir / "data";
			if (!CopyOne(bundleData / "engine.pak", packageData / "engine.pak", error)
			        || !CopyOne(bundleData / "config" / "EngineSettings.toml", packageData / "config" / "EngineSettings.toml", error)
			        || !CopyTree(bundleData / "scripts" / "managed", packageData / "scripts" / "managed", error))
			{
				return Failed("Could not stage the shipped data payload: " + error, "Rebuild the editor so its data folder is complete, then publish again.");
			}
			return {};
		}

		StepResult BakeSettings(const PublishPlan& plan, PublishContext&, const PublishToolchain&)
		{
			const std::filesystem::path settingsPath = plan.outputDir / "data" / "config" / "EngineSettings.toml";
			LoadedEngineSettings loaded = EngineSettingsIO::LoadLayered(settingsPath.string(), plan.projectFile);

			// Never bake a scene the project does not have: a published game has no editor to
			// fall back on, so a dangling name is an empty world with no way to notice.
			std::string error;
			if (!app::ValidateProjectStartupScene(plan.scenesDir, loaded.base.app.startupScene, error))
			{
				return Failed(std::move(error), "Set a startup scene in the Project panel, then publish again.");
			}

			// A shipped runtime has no Play button, so it must boot straight into the scene.
			loaded.base.app.autoplay = true;

			if (auto writeResult = io::file_util::WriteText(settingsPath, EngineSettingsIO::Serialize(loaded.base)); !writeResult)
			{
				return Failed("Could not write the published settings: " + writeResult.error().message, "Check write permissions on the Builds folder.");
			}
			AE_INFO(LogCategory::App, "Baked published settings (startup scene '{}')", loaded.base.app.startupScene);
			return {};
		}

		StepResult BuildScripts(const PublishPlan& plan, PublishContext&, const PublishToolchain& toolchain)
		{
			const std::filesystem::path scriptsProject = plan.scriptsDir / "AetherGame.csproj";
			if (!io::file_util::Exists(scriptsProject))
			{
				return {}; // A project without scripts is fine.
			}
			if (toolchain.dotnetExe.empty())
			{
				return Failed("The project has C# scripts, but this editor was built without .NET publishing support.", "Install the .NET SDK and reconfigure CMake, then publish again.");
			}

			const std::filesystem::path artifactsDir = plan.projectRoot / "Builds" / "Intermediate" / "managed";
			const std::filesystem::path publishLog = plan.projectRoot / "Builds" / "publish-scripts.log";
			std::error_code ec;
			std::filesystem::remove_all(artifactsDir, ec);
			if (ec)
			{
				return Failed("Could not clean script build intermediates: " + ec.message(), "Close anything using the project's Builds folder and publish again.");
			}

			const std::string command = "\"" + toolchain.dotnetExe.string() + "\" build \"" + scriptsProject.string()
			        + "\" -c Release --nologo -v:m -p:DebugSymbols=false -p:DebugType=none -p:Optimize=true -p:ArtifactsPath=\"" + artifactsDir.string() + "\"";
			if (const int rc = io::RunProcessToLog(command, publishLog); rc != 0)
			{
				return Failed("The project's C# scripts failed to build (exit " + std::to_string(rc) + ").", "See " + publishLog.generic_string() + " for the compiler output.");
			}

			std::string error;
			if (!CopyTree(artifactsDir / "bin" / "AetherGame" / "release", plan.outputDir / "data" / "scripts" / "managed", error))
			{
				return Failed("Could not stage the built C# scripts: " + error, "See " + publishLog.generic_string() + ".");
			}
			return {};
		}

		StepResult PruneDevFiles(const PublishPlan& plan, PublishContext&, const PublishToolchain&)
		{
			std::error_code ec;
			for (const auto& entry: std::filesystem::recursive_directory_iterator(plan.outputDir, ec))
			{
				if (ec)
				{
					return Failed("Could not inspect the package: " + ec.message(), "Close anything using the Builds folder and publish again.");
				}
				if (!entry.is_regular_file(ec))
				{
					continue;
				}
				if (IsPrunablePublishedFile(entry.path()) || IsPakSidecarFile(entry.path()))
				{
					std::filesystem::remove(entry.path(), ec);
					if (ec)
					{
						return Failed("Could not remove a dev-only file: " + ec.message(), "Close anything using the Builds folder and publish again.");
					}
				}
			}
			return {};
		}

		StepResult VerifyPackage(const PublishPlan& plan, PublishContext&, const PublishToolchain&)
		{
			if (auto issue = VerifyPublishedPackage(plan.outputDir, plan.runtimeExeName))
			{
				return Failed(std::move(issue->message), std::move(issue->remediation));
			}
			return {};
		}

		StepResult WriteReport(const PublishPlan& plan, PublishContext& context, const PublishToolchain&)
		{
			const PublishReport report = BuildPublishReport(plan);
			context.reportSummary = report.ok ? report.summary : "Published " + plan.productName + ".";
			if (report.ok)
			{
				// Best effort: a missing report never fails a good package.
				if (auto writeResult = io::file_util::WriteText(plan.outputDir / "publish-report.txt", report.text); !writeResult)
				{
					AE_WARN(LogCategory::App, "Could not write publish-report.txt: {}", writeResult.error().message);
				}
			}
			return {};
		}
	} // namespace

	PublishToolchain MakePublishToolchain()
	{
		PublishToolchain toolchain;
#ifdef AETHER_DOTNET_EXE
		toolchain.dotnetExe = AETHER_DOTNET_EXE;
#endif
#ifdef AETHER_MANAGED_CONFIG
		toolchain.managedConfig = AETHER_MANAGED_CONFIG;
#endif
#ifdef AETHER_MANAGED_CONFIGDIR
		toolchain.managedConfigDir = AETHER_MANAGED_CONFIGDIR;
#endif
#ifdef AETHER_MANAGED_SDK_PROJECT
		toolchain.managedSdkProject = AETHER_MANAGED_SDK_PROJECT;
#endif
		return toolchain;
	}

	std::span<const PublishStep> PublishStepList()
	{
		static constexpr PublishStep kSteps[] = {
		        {"Validating project", &ValidateProject},
		        {"Cleaning output", &CleanOutput},
		        {"Packing project assets", &PackProjectAssets},
		        {"Staging game runtime", &StageRuntime},
		        {"Baking game settings", &BakeSettings},
		        {"Building game scripts", &BuildScripts},
		        {"Removing dev files", &PruneDevFiles},
		        {"Verifying package", &VerifyPackage},
		        {"Writing report", &WriteReport},
		};
		return kSteps;
	}
} // namespace aether::editor
