#include "editor/EditorEnginePak.hpp"

#include <string_view>
#include <system_error>

#include "AssetPipeline.hpp"
#include "io/FileUtil.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kEngineAssetSubdirs[] = {"fonts"};
		constexpr std::string_view kEngineAssetFiles[] = {"branding/aethercore-icon-white.png"};

		constexpr std::string_view kEngineShaderSubdir = "shaders";

		std::optional<std::filesystem::path> EngineShaderDir()
		{
#ifdef AETHER_SHADER_BUILD_DIR
			std::error_code ec;
			std::filesystem::path dir = AETHER_SHADER_BUILD_DIR;
			if (std::filesystem::is_directory(dir, ec))
			{
				return dir;
			}
#endif
			return std::nullopt;
		}
	} // namespace

	bool CanBakeEnginePak()
	{
		if (!EngineResourcesDir().has_value())
		{
			return false;
		}

#ifdef AETHER_SHADER_BUILD_DIR
		if (!EngineShaderDir().has_value())
		{
			return false;
		}
#endif

		return true;
	}

	std::optional<std::filesystem::path> EngineResourcesDir()
	{
#ifdef AETHER_ENGINE_RESOURCES_DIR
		std::error_code ec;
		std::filesystem::path dir = AETHER_ENGINE_RESOURCES_DIR;
		if (std::filesystem::is_directory(dir, ec))
		{
			return dir;
		}
#endif
		return std::nullopt;
	}

	EditorProjectActionResult BakeEnginePak(const std::filesystem::path& outputEnginePak)
	{
		const std::optional<std::filesystem::path> resources = EngineResourcesDir();
		if (!resources)
		{
			return {.succeeded = false, .message = "Engine resources are not available in this build; cannot bake engine.pak."};
		}

		std::error_code ec;
		const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			return {.succeeded = false, .message = "Could not resolve temp directory: " + ec.message()};
		}
		const std::filesystem::path staging = tempRoot / "aether_engine_pak_stage";
		std::filesystem::remove_all(staging, ec);
		if (auto dirResult = io::file_util::CreateDirectories(staging); !dirResult)
		{
			return {.succeeded = false, .message = "Could not create engine.pak staging dir: " + dirResult.error().message};
		}

		for (const std::string_view subdir: kEngineAssetSubdirs)
		{
			const std::filesystem::path from = *resources / subdir;
			if (!std::filesystem::is_directory(from, ec))
			{
				continue;
			}
			std::filesystem::copy(from, staging / subdir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
			{
				std::error_code cleanupEc;
				std::filesystem::remove_all(staging, cleanupEc);
				return {.succeeded = false, .message = "Could not stage engine assets '" + std::string(subdir) + "': " + ec.message()};
			}
		}

		for (const std::string_view file: kEngineAssetFiles)
		{
			const std::filesystem::path from = *resources / file;
			const std::filesystem::path to = staging / file;
			std::filesystem::create_directories(to.parent_path(), ec);
			if (!ec)
			{
				std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
			}
			if (ec)
			{
				std::error_code cleanupEc;
				std::filesystem::remove_all(staging, cleanupEc);
				return {.succeeded = false, .message = "Could not stage engine asset '" + std::string(file) + "': " + ec.message()};
			}
		}

		if (const std::optional<std::filesystem::path> shaders = EngineShaderDir())
		{
			std::filesystem::copy(*shaders, staging / kEngineShaderSubdir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
			{
				std::error_code cleanupEc;
				std::filesystem::remove_all(staging, cleanupEc);
				return {.succeeded = false, .message = "Could not stage engine shaders: " + ec.message()};
			}
		}

		const assetpipeline::PackResult result = assetpipeline::PackDirectory(staging, outputEnginePak, {});
		std::filesystem::remove_all(staging, ec);

		if (!result.ok)
		{
			return {.succeeded = false, .message = result.message, .outputPath = outputEnginePak};
		}
		AE_INFO(LogCategory::App, "Baked engine.pak to {}", outputEnginePak.generic_string());
		return {.succeeded = true, .message = "Baked engine.pak (" + std::to_string(result.pakBytes / 1024) + " KB).", .outputPath = outputEnginePak};
	}
} // namespace aether::editor
