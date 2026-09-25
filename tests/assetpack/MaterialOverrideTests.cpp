#include <doctest/doctest.h>

#include <PakFormat.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "AssetProcessor.hpp"

namespace
{
	std::vector<std::byte> ToBytes(const std::string& s)
	{
		const auto* p = reinterpret_cast<const std::byte*>(s.data());
		return std::vector<std::byte>(p, p + s.size());
	}

	// An isolated materials/ tree: the override file sits NEXT TO where the bake writes
	// its <name>.material FILE - that coexistence is the whole point of the form.
	struct OverrideTree
	{
		std::filesystem::path root;
		std::filesystem::path sourceDir;
		std::filesystem::path overrideFile;

		OverrideTree()
		{
			root = std::filesystem::temp_directory_path() / "aether-override-test";
			std::filesystem::remove_all(root);
			sourceDir = root / "model";
			std::filesystem::create_directories(sourceDir / "materials");
			overrideFile = sourceDir / "materials" / "m_sea.material.override.toml";
			std::ofstream out(overrideFile, std::ios::binary);
			out << "[material]\n"
			    << "baseColorFactor = [1.0, 1.0, 1.0, 1.0]\n"
			    << "uvScroll = [0.055, 0.0]\n"
			    << "\n"
			    << "[textures]\n"
			    << "albedo = 'sea.png'\n";
		}

		~OverrideTree()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}
	};
} // namespace

// The baked materials/<name>.material FILE and an override for the same material must be
// able to coexist: the original <name>.material/properties.toml DIRECTORY form loses to
// the file on NTFS (same name), which made overriding an auto-generated material impossible.
TEST_CASE("A <name>.material.override.toml publishes under the baked material's virtual path")
{
	OverrideTree tree;

	const auto raw = [&]
	{
		std::ifstream in(tree.overrideFile, std::ios::binary);
		return ToBytes(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
	}();

	const auto proc = aether::assetpipeline::ProcessAsset(
	        raw, tree.overrideFile, (tree.sourceDir / "materials" / "m_sea.material.override.toml").string(), tree.sourceDir);
	REQUIRE_FALSE(proc.data.empty());
	CHECK(proc.outExt == ".material");

	const auto result = aether::assetpipeline::ProcessFile(
	        (tree.sourceDir / "materials" / "m_sea.material.override.toml").string(), tree.overrideFile, tree.sourceDir, 0);
	REQUIRE(result.ok);
	// Exactly the path the generated material would take - the override replaces it in place.
	CHECK(result.virtualPath == (tree.sourceDir / "materials" / "m_sea.material").generic_string());
	REQUIRE(result.extraFiles.empty());

	// And the payload is the override's content, uvScroll included (offset 56 in MATL).
	REQUIRE(result.data.size() >= 64);
	REQUIRE(result.data[0] == std::byte{'M'});
	const float u = *reinterpret_cast<const float*>(result.data.data() + 56);
	const float v = *reinterpret_cast<const float*>(result.data.data() + 60);
	CHECK(u == doctest::Approx(0.055f));
	CHECK(v == doctest::Approx(0.0f).epsilon(0.0001));
}

TEST_CASE("A properties.toml override still maps through the directory form")
{
	OverrideTree tree;
	const auto dir = tree.sourceDir / "materials" / "m_other.material";
	std::filesystem::create_directories(dir);
	const auto props = dir / "properties.toml";
	{
		std::ofstream out(props, std::ios::binary);
		out << "[material]\n"
		    << "baseColorFactor = [1.0, 1.0, 1.0, 1.0]\n";
	}

	const auto result = aether::assetpipeline::ProcessFile(
	        (dir / "properties.toml").string(), props, tree.sourceDir, 0);
	REQUIRE(result.ok);
	CHECK(result.virtualPath == (tree.sourceDir / "materials" / "m_other.material").generic_string());
}
