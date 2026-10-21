#include "materialgraph/MaterialGraph.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include "gpu/SpirvEntryPoints.hpp"

namespace
{
	int Inspect(const std::filesystem::path& binary)
	{
		std::error_code error;
		if (std::filesystem::file_size(binary, error) > 32 * 1024 * 1024 || error)
		{
			return 1;
		}
		std::ifstream input(binary, std::ios::binary);
		const std::vector<char> bytes(std::istreambuf_iterator<char>(input), {});
		const auto source = std::as_bytes(std::span(bytes));
		const auto graph = aether::gpu::HasSpirvFragmentEntry(source, aether::gpu::kGraphPrepassEntry);
		const auto forward = aether::gpu::HasSpirvFragmentEntry(source, "fragmentMain");
		if (!graph || !forward)
		{
			std::cerr << "Malformed material SPIR-V\n";
			return 1;
		}
		std::cout << "{\"graphPrepassMain\":" << (*graph ? "true" : "false") << ",\"fragmentMain\":" << (*forward ? "true" : "false") << "}\n";
		return 0;
	}

	int Compile(const std::filesystem::path& material, const std::filesystem::path& output, bool native)
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(material, ec);
		if (ec || size > 256 * 1024)
		{
			std::cerr << "WEB_SHADER_SOURCE_BUDGET: material must be readable and at most 256 KiB\n";
			return 1;
		}
		std::ifstream input(material, std::ios::binary);
		const std::string source(std::istreambuf_iterator<char>(input), {});
		std::string error;
		auto generated = aether::editor::CompileWebMaterialSurface(source, error);
		if (native && !generated.empty())
		{
			generated = aether::editor::GenerateMaterialShader(*aether::editor::ParseMaterialGraph(source), error);
		}
		if (generated.empty())
		{
			std::cerr << error << '\n';
			return 1;
		}
		std::ofstream stream(output, std::ios::binary);
		stream << generated;
		if (!stream)
		{
			std::cerr << "WEB_SHADER_OUTPUT: cannot write generated surface\n";
			return 1;
		}
		std::cout << (native ? "Native material shader generated\n" : "Web material surface ABI 1 generated\n");
		return 0;
	}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
	const bool native = argc == 4 && std::filesystem::path(argv[1]) == "--native";
	if (argc != 3 && !native)
	{
		std::cerr << "Usage: WebMaterialCompiler [--native] <material.toml> <output.slang>\n";
		return 2;
	}
	try
	{
		if (argc == 3 && std::filesystem::path(argv[1]) == "--inspect")
		{
			return Inspect(std::filesystem::path(argv[2]));
		}
		return Compile(std::filesystem::path(argv[native ? 2 : 1]), std::filesystem::path(argv[native ? 3 : 2]), native);
	}
	catch (const std::exception& error)
	{
		std::cerr << "WEB_SHADER_GRAPH: " << error.what() << '\n';
		return 1;
	}
}
