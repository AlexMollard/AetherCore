#include <doctest/doctest.h>
#include "scripting/RustGameplayBuild.hpp"
#include "io/FileUtil.hpp"
#include "EngineContentPaths.hpp"
#include <cstdlib>
#include <fstream>
#include <algorithm>

TEST_CASE("Rust build rejects missing manifests before touching a previous artifact")
{
	const auto root = std::filesystem::temp_directory_path() / "aether-rust-build-preflight";
	std::filesystem::create_directories(root / "out");
	REQUIRE(aether::io::file_util::WriteText(root / "out" / "previous.txt", "keep"));
	const auto built = aether::app::scripting::BuildRustGameplay({.manifest = root / "missing" / "Cargo.toml", .artifactsDir = root / "target", .outputDir = root / "out"});
	CHECK_FALSE(built.Succeeded());
	CHECK(built.module.empty());
	CHECK(built.report.toolchains.empty());
	REQUIRE_FALSE(built.report.diagnostics.empty());
	CHECK(built.report.diagnostics.front().code == "RUST_MANIFEST_MISSING");
	CHECK(aether::io::file_util::ReadText(root / "out" / "previous.txt").value() == "keep");
	CHECK(aether::scripting::SerializeAgentReport(built.report));
	std::filesystem::remove_all(root);
}

TEST_CASE("Rust CMake pipeline preserves previous artifacts on a failed compile" * doctest::skip(std::getenv("AETHER_RUST_BUILD_TESTS") == nullptr))
{
	const auto root = std::filesystem::temp_directory_path() / std::filesystem::path(std::u8string(u8"aether rust café & build"));
	std::filesystem::create_directories(root / "src");
	const auto sdk = aether::app::EngineRustGameplaySdkManifest().parent_path().generic_u8string();
	const std::string dependency(sdk.begin(), sdk.end());
	REQUIRE(aether::io::file_util::WriteText(root / "Cargo.toml", "[package]\nname='aether_test_game'\nversion='0.1.0'\nedition='2024'\n[lib]\nname='aether_gameplay'\ncrate-type=['cdylib']\n[dependencies]\naethercore={path='" + dependency + "'}\n"));
	REQUIRE(aether::io::file_util::WriteText(root / "src/lib.rs", "use aethercore::{gameplay_script, Script};\ngameplay_script! { pub struct Player { pub speed: f32 = 3.0 } }\nimpl Script for Player {}\naethercore::export_gameplay!(Player);\n"));
	const aether::app::scripting::RustBuildRequest request{.manifest = root / "Cargo.toml", .artifactsDir = root / "intermediate", .outputDir = root / "out", .timeoutMs = 60000};
	const auto success = aether::app::scripting::BuildRustGameplay(request);
	for (const auto& diagnostic: success.report.diagnostics) { INFO(diagnostic.message); }
	REQUIRE(success.Succeeded());
	REQUIRE(success.report.toolchains.size() == 2);
	const auto rustc = std::find_if(success.report.toolchains.begin(), success.report.toolchains.end(), [](const auto& toolchain) { return toolchain.name == "rustc"; });
	const auto cargo = std::find_if(success.report.toolchains.begin(), success.report.toolchains.end(), [](const auto& toolchain) { return toolchain.name == "cargo"; });
	REQUIRE(rustc != success.report.toolchains.end());
	REQUIRE(cargo != success.report.toolchains.end());
	CHECK_FALSE(rustc->version.empty());
	CHECK_FALSE(cargo->version.empty());
	CHECK(rustc->target.find('-') != std::string::npos);
	CHECK(cargo->target == rustc->target);
	const auto rustcIdentity = aether::io::file_util::ReadText(request.artifactsDir / "logs" / "rustc-version.log");
	const auto cargoIdentity = aether::io::file_util::ReadText(request.artifactsDir / "logs" / "cargo-version.log");
	REQUIRE(rustcIdentity);
	REQUIRE(cargoIdentity);
	CHECK(rustcIdentity->find("rustc " + rustc->version) != std::string::npos);
	CHECK(rustcIdentity->find("host: " + rustc->target) != std::string::npos);
	CHECK(cargoIdentity->find("cargo " + cargo->version) != std::string::npos);
	CHECK(aether::scripting::SerializeAgentReport(success.report));
	const auto previousSize = std::filesystem::file_size(success.module);
	REQUIRE(aether::io::file_util::WriteText(root / "src/lib.rs", "fn bad() { no_such_function(); }\n"));
	const auto failed = aether::app::scripting::BuildRustGameplay(request);
	REQUIRE_FALSE(failed.Succeeded());
	CHECK(failed.module.empty());
	CHECK(failed.report.toolchains.size() == 2);
	CHECK(std::filesystem::file_size(success.module) == previousSize);
	CHECK(std::any_of(failed.report.diagnostics.begin(), failed.report.diagnostics.end(), [](const auto& d) { return d.code == "RUST_COMPILER_ERROR" && d.source.has_value(); }));
	CHECK(aether::scripting::SerializeAgentReport(failed.report));
	std::filesystem::remove_all(root);
}
