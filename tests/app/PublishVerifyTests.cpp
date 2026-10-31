#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <cstdlib>

#include "editor/publish/PublishVerify.hpp"
#include "editor/ShaderCompiler.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "RuntimeProjectSettings.hpp"
#include "AssetPipeline.hpp"
#include "io/PakBackend.hpp"
#include <fstream>

using namespace aether;

namespace
{
#ifdef _WIN32
	constexpr const char* kFixtureHostFxr = "hostfxr.dll";
	constexpr const char* kFixtureCoreClr = "coreclr.dll";
	constexpr const char* kFixtureHostPolicy = "hostpolicy.dll";
#elif defined(__APPLE__)
	constexpr const char* kFixtureHostFxr = "libhostfxr.dylib";
	constexpr const char* kFixtureCoreClr = "libcoreclr.dylib";
	constexpr const char* kFixtureHostPolicy = "libhostpolicy.dylib";
#else
	constexpr const char* kFixtureHostFxr = "libhostfxr.so";
	constexpr const char* kFixtureCoreClr = "libcoreclr.so";
	constexpr const char* kFixtureHostPolicy = "libhostpolicy.so";
#endif
	struct ScopedProductIsolation
	{
		std::string previous = io::PlatformPaths::ReadEnvironmentVariable("AETHER_ISOLATED_PRODUCT");
		ScopedProductIsolation()
		{
#ifdef _WIN32
			_putenv_s("AETHER_ISOLATED_PRODUCT", "1");
#else
			setenv("AETHER_ISOLATED_PRODUCT", "1", 1);
#endif
		}
		~ScopedProductIsolation()
		{
#ifdef _WIN32
			_putenv_s("AETHER_ISOLATED_PRODUCT", previous.c_str());
#else
			if (previous.empty()) { unsetenv("AETHER_ISOLATED_PRODUCT"); }
			else { setenv("AETHER_ISOLATED_PRODUCT", previous.c_str(), 1); }
#endif
		}
	};
	// A package that satisfies the file-presence invariants, so a test only has to break the
	// one thing it is about. The pak-content checks run last and cannot be satisfied by stub
	// files; tests that reach them assert on what they can.
	struct FakePackage
	{
		std::filesystem::path root;

		explicit FakePackage(const std::string& name)
		{
			root = std::filesystem::temp_directory_path() / ("aethercore_publishverify_" + name);
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
			for (const char* rel: {"AetherGame.exe",
			             "aether-product",
			             "THIRD_PARTY_NOTICES.txt",
			             "DISTRIBUTION.md",
			             "dotnet/LICENSE.txt",
			             "dotnet/ThirdPartyNotices.txt",
			             "notices/managed-inventory.json",
			             "dotnet/shared/Microsoft.NETCore.App/10.0.0/System.Private.CoreLib.dll",
			             "data/config/EngineSettings.toml",
			             "data/engine.pak",
			             "data/project.pak",
			             "data/scripts/managed/AetherCore.dll",
			             "data/scripts/managed/AetherCore.Interop.dll",
			             "data/scripts/managed/AetherCore.Interop.deps.json",
			             "data/scripts/managed/AetherCore.Interop.runtimeconfig.json",
			             "data/scripts/managed/AetherGame.dll",
			             "data/scripts/managed/AetherGame.deps.json"})
			{
				Write(rel, std::string_view(rel) == "data/config/EngineSettings.toml" ? "[app]\nstartupScene = \"main\"\nautoplay = true\n" : "x");
			}
			Write(std::string("dotnet/host/fxr/10.0.0/") + kFixtureHostFxr, "runtime fixture");
			Write(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.0/") + kFixtureCoreClr, "runtime fixture");
			Write(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.0/") + kFixtureHostPolicy, "runtime fixture");
		}

		~FakePackage()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		void Write(const std::string& rel, const std::string& contents) const
		{
			const std::filesystem::path path = root / rel;
			REQUIRE(io::file_util::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(io::file_util::WriteText(path, contents).has_value());
		}

		void Remove(const std::string& rel) const
		{
			std::error_code ec;
			std::filesystem::remove(root / rel, ec);
		}
	};
} // namespace

TEST_CASE("VerifyPublishedPackage names the missing file")
{
	const FakePackage package("missing");
	package.Remove("data/scripts/managed/AetherGame.dll");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("AetherGame.dll") != std::string::npos);
	CHECK_FALSE(issue->remediation.empty());
}

TEST_CASE("Published notices preserve managed metadata and fail closed on missing terms")
{
	const FakePackage bundle("notice_source");
	const FakePackage package("notice_destination");
	bundle.Remove("aether-product"); // A raw development editor has no delivery marker.
	package.Remove("aether-product");
	bundle.Write("notices/managed/Microsoft.CodeAnalysis.Common/1.0/microsoft.codeanalysis.common.nuspec", "exact restored metadata");
	bundle.Write("notices/managed/Microsoft.CodeAnalysis.Common/1.0/ThirdPartyNotices.rtf", "exact RTF notices");
	REQUIRE_FALSE(editor::StagePublishedNotices(bundle.root, package.root).has_value());
	const auto marker = io::file_util::ReadText(package.root / "aether-product");
	REQUIRE(marker.has_value());
	CHECK(*marker == "AetherCore delivered product v1\n");
	const auto metadata = package.root / "notices/managed/Microsoft.CodeAnalysis.Common/1.0/microsoft.codeanalysis.common.nuspec";
	REQUIRE(io::file_util::ReadText(metadata).has_value());
	CHECK(*io::file_util::ReadText(metadata) == "exact restored metadata");
	CHECK_FALSE(editor::IsPrunablePublishedFile(metadata));
	CHECK(editor::IsPrunablePublishedFile("Microsoft.CodeAnalysis.dll"));
	bundle.Remove("DISTRIBUTION.md");
	CHECK(editor::StagePublishedNotices(bundle.root, package.root).has_value());
	package.Remove("THIRD_PARTY_NOTICES.txt");
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find("THIRD_PARTY_NOTICES.txt") != std::string::npos);
}

TEST_CASE("Published CSharp products require a delivered runtime rather than system fallback")
{
	const FakePackage package("required_managed_runtime");
	REQUIRE_FALSE(editor::VerifyBundledManagedRuntime(package.root).has_value());
	package.Remove(std::string("dotnet/host/fxr/10.0.0/") + kFixtureHostFxr);
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find("bundled .NET runtime") != std::string::npos);
	CHECK(issue->message.find("system runtime fallback is unavailable") != std::string::npos);
	CHECK_FALSE(issue->remediation.empty());
}

TEST_CASE("Published marker is required and a Rust game needs no managed runtime")
{
	const FakePackage package("required_marker");
	package.Remove("aether-product");
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find("aether-product") != std::string::npos);
	package.Write("aether-product", "AetherCore delivered product v1\n");
	package.Write("data/config/EngineSettings.toml", "[scripting]\nlanguage = \"rust\"\n");
	std::filesystem::remove_all(package.root / "dotnet");
	package.Write(app::ResolveRuntimeGameplayModule({}, app::GameplayLanguage::Rust).generic_string(), "module");
	const auto rustIssue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(rustIssue.has_value());
	CHECK(rustIssue->message.find("engine.pak") != std::string::npos);
}

TEST_CASE("Published runtime admission rejects files split between incomplete framework versions")
{
	const FakePackage package("split_managed_versions");
	package.Write(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.1/") + kFixtureCoreClr, "runtime fixture");
	package.Remove(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.0/") + kFixtureCoreClr);
	package.Write(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.2/") + kFixtureHostPolicy, "runtime fixture");
	package.Remove(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.0/") + kFixtureHostPolicy);
	CHECK(editor::VerifyBundledManagedRuntime(package.root).has_value());
	package.Write(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.3/") + kFixtureCoreClr, "runtime fixture");
	package.Write(std::string("dotnet/shared/Microsoft.NETCore.App/10.0.3/") + kFixtureHostPolicy, "runtime fixture");
	package.Write("dotnet/shared/Microsoft.NETCore.App/10.0.3/System.Private.CoreLib.dll", "runtime fixture");
	CHECK_FALSE(editor::VerifyBundledManagedRuntime(package.root).has_value());
}

TEST_CASE("Isolated shader compilation rejects developer compiler and include fallbacks")
{
	const ScopedProductIsolation isolation;
	const FakePackage project("isolated_shader_prerequisites");
	project.Write("assets/shaders/Custom.slang", "[shader(\"compute\")] [numthreads(1,1,1)] void main() {}\n");
	// EngineTests is not a shader-authoring bundle. Compile-time developer tools may
	// exist, but they must not make this isolated capability query succeed.
	REQUIRE_FALSE(editor::CanCompileShaders());
	std::string error;
	CHECK_FALSE(editor::CompileOne(project.root / "assets/shaders/Custom.slang", project.root / "Builds/shaders", error));
	CHECK(error.find("bundled slangc") != std::string::npos);
	const auto result = editor::CompileProject(project.root);
	CHECK_FALSE(result.ok);
	CHECK(result.failed == 1);
	CHECK(result.message.find("prerequisites") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects unreadable startup scene content")
{
	const FakePackage package("unreadable_scene");
	const auto source = package.root / "pak_source";
	REQUIRE(io::file_util::CreateDirectories(source / "shaders").has_value());
	REQUIRE(io::file_util::WriteText(source / "shaders/test.spv", "shader bytes").has_value());
	REQUIRE(assetpipeline::PackDirectory(source, package.root / "data/engine.pak", {}).ok);
	std::filesystem::remove(package.root / "data/engine.pak.log");
	std::filesystem::remove(package.root / "data/engine.pak.manifest");
	std::filesystem::remove_all(source);
	REQUIRE(io::file_util::CreateDirectories(source / "scenes").has_value());
	REQUIRE(io::file_util::WriteText(source / "scenes/main.scene.toml", "[scene]\nname='main'\n").has_value());
	REQUIRE(assetpipeline::PackDirectory(source, package.root / "data/project.pak", {}).ok);
	std::filesystem::remove(package.root / "data/project.pak.log");
	std::filesystem::remove(package.root / "data/project.pak.manifest");
	std::filesystem::remove_all(source);
	REQUIRE_FALSE(editor::VerifyPublishedPackage(package.root, "AetherGame.exe").has_value());

	// Corrupt payload bytes while preserving the index and its validated file bounds.
	std::fstream pak(package.root / "data/project.pak", std::ios::binary | std::ios::in | std::ios::out);
	PakHeader header{};
	pak.read(reinterpret_cast<char*>(&header), sizeof(header));
	std::vector<PakEntry> entries(header.numEntries);
	pak.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(PakEntry)));
	std::string paths(static_cast<std::size_t>(header.pathDataSize), '\0');
	pak.seekg(static_cast<std::streamoff>(header.pathDataOffset));
	pak.read(paths.data(), static_cast<std::streamsize>(paths.size()));
	bool corrupted = false;
	for (const auto& entry: entries)
	{
		if (paths.substr(entry.pathOffset, entry.pathLen) == "scenes/main.scene.toml")
		{
			pak.seekp(static_cast<std::streamoff>(header.assetDataOffset + entry.dataOffset));
			pak.put('!');
			corrupted = true;
			break;
		}
	}
	pak.close();
	REQUIRE(corrupted);
	const io::PakBackend backend(package.root / "data/project.pak");
	REQUIRE_FALSE(backend.Read("scenes/main.scene.toml").has_value());
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find("main") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects source files in the package")
{
	const FakePackage package("source");
	package.Write("data/scripts/PlayerController.cs", "class X {}");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("PlayerController.cs") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects a packer sidecar")
{
	const FakePackage package("sidecar");
	package.Write("data/project.pak.log", "packed 3 files");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("project.pak.log") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects the Aftermath DLL")
{
	const FakePackage package("aftermath");
	package.Write("GFSDK_Aftermath_Lib.x64.dll", "binary");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("Aftermath") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects the editor executable")
{
	const FakePackage package("editor");
	package.Write("Editor.exe", "binary");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("Editor.exe") != std::string::npos);
}

// A debug CRT DLL is NOT a verification failure. Build configuration is reported from
// AE_CONFIG_NAME instead, which a Debug publish cannot contradict.
TEST_CASE("VerifyPublishedPackage accepts a debug CRT DLL")
{
	const FakePackage package("debugcrt");
	package.Write("msvcp140d.dll", "binary");
	package.Write("ucrtbased.dll", "binary");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	// It still fails on the stub engine.pak, but never because of the CRT DLLs.
	if (issue.has_value())
	{
		CHECK(issue->message.find("msvcp140d") == std::string::npos);
		CHECK(issue->message.find("ucrtbased") == std::string::npos);
	}
}

TEST_CASE("IsPrunablePublishedFile covers build leftovers but not shipped content")
{
	CHECK(editor::IsPrunablePublishedFile("AetherGame.pdb"));
	CHECK(editor::IsPrunablePublishedFile("Engine.lib"));
	CHECK(editor::IsPrunablePublishedFile("Editor.ilk"));
	CHECK_FALSE(editor::IsPrunablePublishedFile("AetherGame.exe"));
	CHECK_FALSE(editor::IsPrunablePublishedFile("data/engine.pak"));
}

TEST_CASE("IsForbiddenPublishedFile covers source that must never ship")
{
	CHECK(editor::IsForbiddenPublishedFile("Player.cs"));
	CHECK(editor::IsForbiddenPublishedFile("AetherGame.csproj"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("AetherGame.dll"));
}

TEST_CASE("VerifyPublishedPackage requires the Rust native module selected by baked settings")
{
	const FakePackage package("rust_missing");
	package.Write("data/config/EngineSettings.toml", "[scripting]\nlanguage = \"rust\"\n");
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find(app::ResolveRuntimeGameplayModule({}, app::GameplayLanguage::Rust).generic_string()) != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage accepts a Rust payload without managed gameplay dependencies")
{
	const FakePackage package("rust_payload");
	package.Write("data/config/EngineSettings.toml", "[scripting]\nlanguage = \"rust\"\n");
	for (const char* file: {"AetherCore.dll", "AetherCore.Interop.dll", "AetherCore.Interop.deps.json", "AetherCore.Interop.runtimeconfig.json", "AetherGame.dll", "AetherGame.deps.json"})
	{
		package.Remove(std::string("data/scripts/managed/") + file);
	}
	package.Write(app::ResolveRuntimeGameplayModule({}, app::GameplayLanguage::Rust).generic_string(), "module");
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find("engine.pak") != std::string::npos);
	CHECK(issue->message.find("managed") == std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects invalid baked gameplay language")
{
	const FakePackage package("invalid_language");
	for (const std::string& text: {std::string("[scripting]\nlanguage = \"python\"\n"), std::string("[scripting]\nlanguage = 12\n"), std::string("not toml")})
	{
		package.Write("data/config/EngineSettings.toml", text);
		const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
		REQUIRE(issue.has_value());
		CHECK(issue->message.find("scripting") != std::string::npos);
		CHECK_FALSE(issue->remediation.empty());
	}
}

TEST_CASE("IsForbiddenPublishedFile covers Rust sources and crate build artifacts")
{
	CHECK(editor::IsForbiddenPublishedFile("player.rs"));
	CHECK(editor::IsForbiddenPublishedFile("Cargo.toml"));
	CHECK(editor::IsForbiddenPublishedFile("Cargo.lock"));
	CHECK(editor::IsForbiddenPublishedFile("libaether_gameplay.rlib"));
	CHECK(editor::IsForbiddenPublishedFile("libaether_gameplay.rmeta"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("aether_gameplay.dll"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("libaether_gameplay.so"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("EngineSettings.toml"));
}
