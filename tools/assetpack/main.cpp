#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "MeshProcessor.hpp"
#include "AssetPipeline.hpp"
#include "FontProcessor.hpp"
#include "KenneyImport.hpp"
#include "MaterialImporter.hpp"

using namespace aether::assetpipeline;

namespace fs = std::filesystem;

struct Args
{
	bool project = false;
	bool importMaterials = false;
	int compressionLevel = 3;
	fs::path sourceDir;
	fs::path outputPath;
};

static std::optional<Args> ParseArgs(int argc, char* argv[])
{
	Args args;
	int argOffset = 1;

	while (argOffset < argc)
	{
		const std::string arg = argv[argOffset];

		if (arg == "--import-materials")
		{
			args.importMaterials = true;
			++argOffset;
		}
		else if (arg == "--project" || arg == "pack-project")
		{
			args.project = true;
			++argOffset;
		}
		else if (arg == "--compress-level")
		{
			if (argOffset + 1 >= argc)
			{
				std::cerr << "AssetPacker: --compress-level requires a value (0-22)\n";
				return std::nullopt;
			}
			const std::string val = argv[argOffset + 1];
			auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), args.compressionLevel);
			if (ec != std::errc{})
			{
				std::cerr << "AssetPacker: invalid compression level '" << val << "'\n";
				return std::nullopt;
			}
			argOffset += 2;
		}
		else if (arg == "import-materials")
		{
			if (argc < argOffset + 2)
			{
				std::cerr << "Usage: AssetPacker import-materials <source-dir>\n";
				return std::nullopt;
			}
			const int result = MaterialImporter::ImportDirectory(fs::path(argv[argOffset + 1]));
			std::exit((result < 0) ? 1 : 0);
		}
		else if (arg == "bake-font")
		{
			if (argc < argOffset + 3)
			{
				std::cerr << "Usage: AssetPacker bake-font <ttf> <outDir>\n";
				return std::nullopt;
			}
			const fs::path ttfPath(argv[argOffset + 1]);
			const fs::path outDir(argv[argOffset + 2]);
			const FontProcessor::BakeResult result = FontProcessor::BakeFont(ttfPath, outDir);
			if (!result.success)
			{
				std::cerr << "AssetPacker: bake-font failed: " << result.error << "\n";
				std::exit(1);
			}
			std::cout << "AssetPacker: baked '" << ttfPath.generic_string() << "' -> " << result.glyphCount << " glyphs, " << result.curveCount << " curves, " << result.textureWidth << "x" << result.textureHeight << " curve texture\n";
			std::exit(0);
		}
		else
		{
			break;
		}
	}

	if (argc < argOffset + 2)
	{
		std::cerr << "Usage: AssetPacker [--project] [--import-materials] [--compress-level N] <source-dir> <output.pak>\n";
		std::cerr << "       AssetPacker import-materials <source-dir>\n";
		std::cerr << "       AssetPacker bake-font <ttf> <outDir>\n";
		std::cerr << "       AssetPacker kenney list-packs|list-models|import ... (see 'AssetPacker kenney' with no args)\n";
		return std::nullopt;
	}

	args.sourceDir = fs::path(argv[argOffset]);
	args.outputPath = fs::path(argv[argOffset + 1]);

	return args;
}

namespace kenney = aether::assetpipeline::kenney;

namespace
{
	void PrintKenneyUsage()
	{
		std::cerr << "Usage: AssetPacker kenney list-packs <manifest.toml>\n";
		std::cerr << "       AssetPacker kenney list-models <manifest.toml> <slug> <cacheDir>\n";
		std::cerr << "       AssetPacker kenney import <manifest.toml> <slug> <zipMemberPath> <projectRoot> <category> <propName> <displayName> <mass> <colliderShape> [cacheDir]\n";
	}

	// One JSON object (or array) per invocation on stdout, via nlohmann::json - the
	// established convention for structured data in this codebase (the Editor's own
	// ControlServer methods and undo commands all speak nlohmann::json; a hand-rolled
	// delimited text format would be a second, competing convention for no reason once the
	// library is already a dependency). The AetherCore MCP's Python wrapper parses this
	// directly with `json.loads`; the Editor panel skips this CLI boundary entirely and
	// calls the KenneyImport functions in-process, so JSON never touches that path.
	int RunKenneyCommand(int argc, char* argv[], int argOffset)
	{
		if (argc < argOffset + 1)
		{
			PrintKenneyUsage();
			return 1;
		}
		const std::string sub = argv[argOffset];
		++argOffset;

		if (sub == "list-packs")
		{
			if (argc < argOffset + 1)
			{
				PrintKenneyUsage();
				return 1;
			}
			std::string error;
			const auto packs = kenney::LoadManifest(fs::path(argv[argOffset]), error);
			if (!error.empty())
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", error}}.dump() << "\n";
				return 1;
			}
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& p: packs)
			{
				arr.push_back({
				        {"slug", p.slug},
				        {"name", p.name},
				        {"version", p.version},
				        {"license", p.license},
				        {"licenseUrl", p.licenseUrl},
				        {"author", p.author},
				        {"pageUrl", p.pageUrl},
				        {"modelDir", p.modelDir},
				        {"previewImageUrl", p.previewImageUrl},
				});
			}
			std::cout << nlohmann::json{{"ok", true}, {"packs", arr}}.dump() << "\n";
			return 0;
		}

		if (sub == "list-models")
		{
			if (argc < argOffset + 3)
			{
				PrintKenneyUsage();
				return 1;
			}
			std::string error;
			const auto packs = kenney::LoadManifest(fs::path(argv[argOffset]), error);
			if (!error.empty())
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", error}}.dump() << "\n";
				return 1;
			}
			const auto pack = kenney::FindPack(packs, argv[argOffset + 1]);
			if (!pack)
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", "unknown pack slug '" + std::string(argv[argOffset + 1]) + "'"}}.dump() << "\n";
				return 1;
			}
			const kenney::CacheResult cache = kenney::EnsurePackCached(*pack, fs::path(argv[argOffset + 2]));
			if (!cache.ok)
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", cache.error}}.dump() << "\n";
				return 1;
			}
			const auto entries = kenney::ListPackModels(cache.zipPath, pack->modelDir, error);
			if (!error.empty())
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", error}}.dump() << "\n";
				return 1;
			}
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& e: entries)
			{
				arr.push_back({{"zipMemberPath", e.zipMemberPath}, {"fileName", e.fileName}});
			}
			std::cout << nlohmann::json{{"ok", true}, {"packCached", cache.wasAlreadyCached}, {"models", arr}}.dump() << "\n";
			return 0;
		}

		if (sub == "import")
		{
			if (argc < argOffset + 9)
			{
				PrintKenneyUsage();
				return 1;
			}
			std::string error;
			const auto packs = kenney::LoadManifest(fs::path(argv[argOffset]), error);
			if (!error.empty())
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", error}}.dump() << "\n";
				return 1;
			}
			const auto pack = kenney::FindPack(packs, argv[argOffset + 1]);
			if (!pack)
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", "unknown pack slug '" + std::string(argv[argOffset + 1]) + "'"}}.dump() << "\n";
				return 1;
			}

			kenney::ImportRequest request;
			request.pack = *pack;
			request.zipMemberPath = argv[argOffset + 2];
			request.projectRoot = fs::path(argv[argOffset + 3]);
			request.category = argv[argOffset + 4];
			request.propName = argv[argOffset + 5];
			request.displayName = argv[argOffset + 6];
			try
			{
				request.mass = std::stof(argv[argOffset + 7]);
			}
			catch (const std::exception&)
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", "invalid mass '" + std::string(argv[argOffset + 7]) + "'"}}.dump() << "\n";
				return 1;
			}
			const auto shape = kenney::ParseColliderShape(argv[argOffset + 8]);
			if (!shape)
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", "invalid colliderShape '" + std::string(argv[argOffset + 8]) + "' (want auto|box|sphere|capsule|cylinder|none)"}}.dump() << "\n";
				return 1;
			}
			request.requestedShape = *shape;
			request.cacheDir = argc > argOffset + 9 ? fs::path(argv[argOffset + 9]) : fs::path(".temp/kenney-cache");

			// MeshProcessor/MaterialImporter print human-readable progress straight to
			// stdout (e.g. "+ auto-generated .material for 'x'") - fine for AssetPacker's
			// normal pack command run in a terminal, but this subcommand's contract is
			// exactly one JSON line on stdout. Capture and relay it to stderr instead of
			// discarding it, so a failure still has that context on hand.
			std::ostringstream suppressed;
			std::streambuf* const prevStdout = std::cout.rdbuf(suppressed.rdbuf());
			const kenney::ImportResult result = kenney::ImportModel(request);
			std::cout.rdbuf(prevStdout);
			if (!suppressed.str().empty())
			{
				std::cerr << suppressed.str();
			}
			if (!result.ok)
			{
				std::cout << nlohmann::json{{"ok", false}, {"error", result.error}}.dump() << "\n";
				return 1;
			}
			const nlohmann::json out = {
			        {"ok", true},
			        {"modelPath", result.modelPath.generic_string()},
			        {"modelAlreadyPresent", result.modelAlreadyPresent},
			        {"texturePath", result.texturePath.generic_string()},
			        {"textureAlreadyPresent", result.textureAlreadyPresent},
			        {"bakedNow", result.bakedNow},
			        {"collider",
			                {
			                        {"shape", kenney::ToString(result.collider.shape)},
			                        {"halfExtents", {result.collider.halfExtents.x, result.collider.halfExtents.y, result.collider.halfExtents.z}},
			                        {"radius", result.collider.radius},
			                        {"halfHeight", result.collider.halfHeight},
			                        {"center", {result.collider.center.x, result.collider.center.y, result.collider.center.z}},
			                        {"nativeSize", {result.collider.nativeSize.x, result.collider.nativeSize.y, result.collider.nativeSize.z}},
			                }},
			        {"creditsLine", result.creditsLine},
			        {"creditsAppended", result.creditsAppended},
			        {"creditsAlreadyPresent", result.creditsAlreadyPresent},
			        {"catalogEntry", result.catalogEntry},
			        {"catalogAppended", result.catalogAppended},
			        {"catalogAlreadyPresent", result.catalogAlreadyPresent},
			        {"catalogSkippedNoCollider", result.catalogSkippedNoCollider},
			        {"warnings", result.warnings},
			};
			std::cout << out.dump() << "\n";
			return 0;
		}

		PrintKenneyUsage();
		return 1;
	}
} // namespace

static int RunBakeCommand(int argc, char* argv[])
{
	if (argc < 4)
	{
		std::cerr << "Usage: AssetPacker bake <gltf-or-glb-path> <project-root>\n";
		return 1;
	}
	const fs::path modelPath = argv[2];
	const fs::path projectRoot = argv[3];
	std::error_code ec;
	const auto size = fs::file_size(modelPath, ec);
	if (ec)
	{
		std::cerr << "AssetPacker bake: cannot stat '" << modelPath.string() << "'\n";
		return 1;
	}
	std::vector<std::byte> raw(size);
	{
		std::ifstream in(modelPath, std::ios::binary);
		if (!in || !in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size)))
		{
			std::cerr << "AssetPacker bake: cannot read '" << modelPath.string() << "'\n";
			return 1;
		}
	}
	const fs::path rel = fs::relative(modelPath, projectRoot);
	const auto result = MeshProcessor::Process(std::span<const std::byte>(raw.data(), raw.size()), modelPath, rel.generic_string(), projectRoot);
	if (result.meshData.empty())
	{
		std::cerr << "AssetPacker bake: no mesh geometry produced from '" << modelPath.string() << "'\n";
		return 1;
	}
	const std::string stem = modelPath.stem().generic_string();
	const fs::path modelDir = modelPath.parent_path();
	auto writeOut = [](const fs::path& outPath, const ByteBuffer& data)
	{
		if (data.empty())
		{
			return;
		}
		std::ofstream out(outPath, std::ios::binary);
		out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		std::cout << "wrote " << outPath.string() << " (" << data.size() << " bytes)\n";
	};
	writeOut(modelDir / (stem + ".mesh"), result.meshData);
	writeOut(modelDir / (stem + ".skel"), result.skelData);
	writeOut(modelDir / (stem + ".animset"), result.animsetData);
	for (const auto& [fileName, animData]: result.animFiles)
	{
		fs::create_directories(projectRoot / "animations");
		writeOut(projectRoot / "animations" / fileName, animData);
	}
	for (const auto& [matVfsPath, matData]: result.materialFiles)
	{
		const fs::path matDisk = projectRoot / matVfsPath;
		fs::create_directories(matDisk.parent_path());
		writeOut(matDisk, matData);
	}
	return 0;
}

int main(int argc, char* argv[])
{
	if (argc >= 2 && std::string(argv[1]) == "kenney")
	{
		return RunKenneyCommand(argc, argv, 2);
	}
	if (argc >= 2 && std::string(argv[1]) == "bake")
	{
		return RunBakeCommand(argc, argv);
	}
	const auto args = ParseArgs(argc, argv);
	if (!args)
	{
		return 1;
	}

	const PackOptions options{
	        .compressionLevel = args->compressionLevel,
	        .importMaterials = args->importMaterials,
	        .projectLayout = args->project,
	};
	const PackResult result = args->project ? PackProject(args->sourceDir, args->outputPath, options) : PackDirectory(args->sourceDir, args->outputPath, options);
	if (!result.ok)
	{
		std::cerr << "AssetPacker: " << result.message << "\n";
		return 1;
	}
	return 0;
}
