#include "scene/SceneDocumentExpansion.hpp"
#include "scene/SceneStreamingDependencies.hpp"
#include "scene/SceneSerializer.hpp"
#include "io/FileSystem.hpp"
#include "platform/web/WebFileSystem.hpp"
#include <TextureMips.hpp>
#include <algorithm>
#include <emscripten/emscripten.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	using namespace aether::app::scene;
	std::unordered_map<std::string, std::vector<std::byte>> files;
	std::unordered_map<std::string, std::string> aliases;
	std::vector<std::byte> output;
	std::vector<std::byte> dependencyOutput;
	std::string error;
	bool catalogPresent = false, catalogValid = false;
	constexpr std::size_t kMaxDocuments = 4096, kMaxBytes = 4u * 1024u * 1024u, kMaxOutput = 32u * 1024u * 1024u;
	std::size_t outputLimit = kMaxOutput;
	std::string scenesDir, prefabsDir;
	std::size_t reads = 0;

	std::string Resolve(std::string_view ref, SceneDocumentKind kind)
	{
		std::string value(ref);
		if (auto it = aliases.find(value); it != aliases.end())
		{
			value = it->second;
		}
		if (value.starts_with("uid://"))
		{
			return {};
		}
		if (!value.starts_with("project://"))
		{
			if (value.empty() || value.size() > 128 || value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
			{
				return {};
			}
			value = "project://" + (kind == SceneDocumentKind::Scene ? scenesDir : prefabsDir) + "/" + value + (kind == SceneDocumentKind::Scene ? ".scene.toml" : ".prefab.toml");
		}
		const auto relative = value.substr(10);
		if (relative.empty() || relative.find("..") != std::string::npos || relative.find('\\') != std::string::npos || relative.find('\0') != std::string::npos)
		{
			return {};
		}
		return value;
	}

	std::expected<SceneDescription, std::string> Read(std::string_view reference, SceneDocumentKind kind)
	{
		if (++reads > kMaxDocuments)
		{
			return std::unexpected("Scene document count exceeded");
		}
		auto path = Resolve(reference, kind);
		auto it = files.find(path);
		if (path.empty() || it == files.end())
		{
			return std::unexpected("Missing scene document: " + path);
		}
		if (it->second.size() > kMaxBytes)
		{
			return std::unexpected("Scene document exceeds limit: " + path);
		}
		std::optional<SceneDescription> scene;
		if (path.ends_with(".bin"))
		{
			scene = ReadSceneBinary(it->second);
		}
		else
		{
			scene = ParseToml(std::string_view(reinterpret_cast<const char*>(it->second.data()), it->second.size()));
		}
		if (!scene && !catalogValid)
		{
			const std::string_view data(reinterpret_cast<const char*>(it->second.data()), it->second.size());
			if (data.find("uid://") != std::string_view::npos)
			{
				return std::unexpected(std::string(catalogPresent ? "Invalid" : "Missing") + " asset catalog required by scene: " + path);
			}
		}
		if (!scene || scene->version < 1 || scene->version > kSceneFormatVersion || !scene->streamingRegions.empty() || scene->entities.size() > 100000)
		{
			return std::unexpected("Invalid or nested streaming scene: " + path);
		}
		return std::move(*scene);
	}

	bool Append(const std::vector<std::byte>& bytes)
	{
		if (bytes.size() > UINT32_MAX || output.size() > outputLimit - 4 || bytes.size() > outputLimit - output.size() - 4)
		{
			return false;
		}
		const auto n = static_cast<std::uint32_t>(bytes.size());
		for (unsigned i = 0; i < 4; ++i)
		{
			output.push_back(static_cast<std::byte>((n >> (i * 8)) & 255));
		}
		output.insert(output.end(), bytes.begin(), bytes.end());
		return true;
	}

	bool Encode(SceneDescription scene)
	{
		auto entities = std::move(scene.entities);
		scene.entities.clear();
		output = {std::byte{'A'}, std::byte{'E'}, std::byte{'S'}, std::byte{'C'}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
		const auto count = static_cast<std::uint32_t>(entities.size() + 1);
		for (unsigned i = 0; i < 4; ++i)
		{
			output.push_back(static_cast<std::byte>((count >> (i * 8)) & 255));
		}
		for (unsigned i = 0; i < 4; ++i)
		{
			output.push_back(std::byte{0});
		}
		if (!Append(WriteSceneBinary(scene)))
		{
			error = "Prepared scene exceeds output limit";
			output.clear();
			return false;
		}
		SceneDescription record;
		record.version = scene.version;
		record.kind = scene.kind;
		record.features = scene.features;
		for (auto& entity: entities)
		{
			record.entities.clear();
			record.entities.push_back(std::move(entity));
			if (!Append(WriteSceneBinary(record)))
			{
				error = "Prepared scene exceeds output limit";
				output.clear();
				return false;
			}
		}
		return true;
	}
} // namespace

extern "C"
{
	EMSCRIPTEN_KEEPALIVE void stream_parser_reset()
	{
		aether::io::FileSystem::Shutdown();
		files.clear();
		aliases.clear();
		output.clear();
		dependencyOutput.clear();
		error.clear();
		reads = 0;
		catalogPresent = false;
		catalogValid = false;
	}

	EMSCRIPTEN_KEEPALIVE int stream_parser_mount(const char* path, const std::byte* data, unsigned size)
	{
		if (!path || !data || size > kMaxBytes || files.size() >= kMaxDocuments)
		{
			return 0;
		}
		if (std::string_view(path) == "project://asset-catalog.toml")
		{
			if (catalogPresent)
			{
				return 0;
			}
			catalogPresent = true;
			auto mounted = aether::web::MountWebAsset(path, std::vector<std::byte>(data, data + size));
			catalogValid = mounted && aether::io::FileSystem::ReloadAssetCatalog().has_value();
			return 1;
		}
		return files.emplace(path, std::vector<std::byte>(data, data + size)).second ? 1 : 0;
	}

	EMSCRIPTEN_KEEPALIVE int stream_parser_alias(const char* alias, const char* path)
	{
		if (!alias || !path || aliases.size() >= 8192)
		{
			return 0;
		}
		return aliases.emplace(alias, path).second ? 1 : 0;
	}

	EMSCRIPTEN_KEEPALIVE int stream_parser_prepare(const char* root, const char* sceneDir, const char* prefabDir, unsigned maxOutput)
	{
		output.clear();
		error.clear();
		reads = 0;
		scenesDir = sceneDir ? sceneDir : "scenes";
		prefabsDir = prefabDir ? prefabDir : "assets/prefabs";
		auto normalize = [](std::string& directory)
		{
			std::replace(directory.begin(), directory.end(), '\\', '/');
			const std::filesystem::path path(directory);
			if (path.is_absolute() || directory.find("..") != std::string::npos)
			{
				return false;
			}
			directory = path.lexically_normal().generic_string();
			return !directory.empty() && directory != ".";
		};
		if (!normalize(scenesDir) || !normalize(prefabsDir))
		{
			error = "Invalid scene or prefab directory";
			return 0;
		}
		if (!root)
		{
			error = "Missing scene reference";
			return 0;
		}
		if (maxOutput < 16 || maxOutput > kMaxOutput)
		{
			error = "Invalid prepared scene output limit";
			return 0;
		}
		outputLimit = maxOutput;
		auto scene = Read(root, SceneDocumentKind::Scene);
		if (!scene)
		{
			error = scene.error();
			return 0;
		}
		scene->name = root;
		auto expanded = ExpandSceneDocument(*scene, Read, {.maxDepth = 32, .maxEntities = 100000, .preserveExternalNodeReferences = true});
		if (!expanded)
		{
			error = expanded.error();
			return 0;
		}
		if (auto ids = EnsureStreamingSceneNodeIds(*expanded); !ids)
		{
			error = ids.error();
			return 0;
		}
		return Encode(std::move(*expanded)) ? 1 : 0;
	}

	EMSCRIPTEN_KEEPALIVE int stream_parser_prepare_snapshot(const char* root, unsigned maxOutput)
	{
		output.clear();
		dependencyOutput.clear();
		error.clear();
		reads = 0;
		if (!root || maxOutput < 16 || maxOutput > kMaxOutput)
		{
			error = "Invalid snapshot preparation";
			return 0;
		}
		outputLimit = maxOutput;
		auto scene = Read(root, SceneDocumentKind::Scene);
		if (!scene)
		{
			error = scene.error();
			return 0;
		}
		if (!scene->includes.empty() || !scene->prefabInstances.empty() || !scene->streamingRegions.empty())
		{
			error = "Snapshot contains unexpanded scene links";
			return 0;
		}
		std::vector<std::string> dependencies;
		auto scan = CollectSceneStreamingDependencies(*scene,
		        dependencies,
		        [](std::string_view reference)
		        {
			        std::string path(reference);
			        if (auto it = aliases.find(path); it != aliases.end())
			        {
				        path = it->second;
			        }
			        if (!path.starts_with("project://") && !path.starts_with("engine://"))
			        {
				        return std::string{};
			        }
			        if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos || path.find('\0') != std::string::npos)
			        {
				        return std::string{};
			        }
			        return path;
		        });
		if (!scan)
		{
			error = scan.error();
			return 0;
		}
		constexpr std::size_t maxDependencies = 1024u * 1024u;
		const auto write32 = [](std::uint32_t n)
		{
			for (unsigned i = 0; i < 4; ++i)
			{
				dependencyOutput.push_back(static_cast<std::byte>((n >> (i * 8)) & 255));
			}
		};
		if (dependencies.size() > 65536)
		{
			error = "Snapshot dependency count exceeds limit";
			return 0;
		}
		write32(static_cast<std::uint32_t>(dependencies.size()));
		for (const auto& path: dependencies)
		{
			if (path.size() > 4096 || dependencyOutput.size() + 4 + path.size() > maxDependencies)
			{
				error = "Snapshot dependencies exceed limit";
				return 0;
			}
			write32(static_cast<std::uint32_t>(path.size()));
			const auto* first = reinterpret_cast<const std::byte*>(path.data());
			dependencyOutput.insert(dependencyOutput.end(), first, first + path.size());
		}
		return Encode(std::move(*scene)) ? 1 : 0;
	}

	EMSCRIPTEN_KEEPALIVE int stream_parser_build_mips(const std::uint8_t* pixels, unsigned size, unsigned width, unsigned height)
	{
		output.clear();
		error.clear();
		if (!pixels || !width || !height || width > 4096 || height > 4096 || static_cast<std::uint64_t>(width) * height * 4 != size)
		{
			error = "Invalid mip source";
			return 0;
		}
		const auto plan = aether::PlanRgbaTextureMips(width, height, true, kMaxOutput);
		if (!plan || 20ull + 2ull * (12ull * plan->levelCount + plan->totalBytes) > kMaxOutput)
		{
			error = "Mip chain exceeds limit";
			return 0;
		}
		output.reserve(static_cast<std::size_t>(20ull + 2ull * (12ull * plan->levelCount + plan->totalBytes)));
		output.insert(output.end(), {std::byte{'A'}, std::byte{'E'}, std::byte{'M'}, std::byte{'I'}});
		const auto appendU32 = [](std::uint32_t n)
		{
			for (unsigned i = 0; i < 4; ++i)
			{
				output.push_back(static_cast<std::byte>((n >> (i * 8)) & 255));
			}
		};
		appendU32(1);
		appendU32(width);
		appendU32(height);
		appendU32(plan->levelCount);
		for (bool srgb: {false, true})
		{
			const auto levels = aether::BuildRgbaTextureMips(std::span<const std::uint8_t>(pixels, size), width, height, true, kMaxOutput, srgb);
			if (!levels)
			{
				error = "Mip chain preparation failed";
				output.clear();
				return 0;
			}
			for (const auto& level: *levels)
			{
				appendU32(level.width);
				appendU32(level.height);
				appendU32(static_cast<std::uint32_t>(level.pixels.size()));
				const auto* first = reinterpret_cast<const std::byte*>(level.pixels.data());
				output.insert(output.end(), first, first + level.pixels.size());
			}
		}
		return 1;
	}

	EMSCRIPTEN_KEEPALIVE const std::byte* stream_parser_data()
	{
		return output.data();
	}

	EMSCRIPTEN_KEEPALIVE unsigned stream_parser_size()
	{
		return static_cast<unsigned>(output.size());
	}

	EMSCRIPTEN_KEEPALIVE const std::byte* stream_parser_dependencies_data()
	{
		return dependencyOutput.data();
	}

	EMSCRIPTEN_KEEPALIVE unsigned stream_parser_dependencies_size()
	{
		return static_cast<unsigned>(dependencyOutput.size());
	}

	EMSCRIPTEN_KEEPALIVE const char* stream_parser_error()
	{
		return error.c_str();
	}
}
