#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>

#include "materialgraph/MaterialGraph.hpp"

using namespace aether::editor;

TEST_CASE("A default graph generates a shader with both entry points")
{
	std::string error;
	const std::string shader = GenerateMaterialShader(MakeDefaultMaterialGraph(), error);
	CHECK(error.empty());
	CHECK(shader.find("fragmentMain") != std::string::npos);
	// The vertex stage comes from the engine header rather than being generated.
	CHECK(shader.find("DefaultVertex.slangh") != std::string::npos);
	CHECK(shader.find("0.6") != std::string::npos);
}

TEST_CASE("A graph with no Output node is refused rather than generated")
{
	MaterialGraph graph;
	graph.nodes.push_back(MaterialNode{.id = 1, .type = MaterialNodeType::ConstantColor});
	std::string error;
	CHECK(GenerateMaterialShader(graph, error).empty());
	CHECK(!error.empty());
}

// A cycle used to recurse until the stack ran out; it has to come back as an error.
TEST_CASE("A cycle is reported instead of overflowing the stack")
{
	MaterialGraph graph;
	graph.nodes.push_back(MaterialNode{.id = 1, .type = MaterialNodeType::Output});
	graph.nodes.push_back(MaterialNode{.id = 2, .type = MaterialNodeType::Multiply});
	graph.nodes.push_back(MaterialNode{.id = 3, .type = MaterialNodeType::Add});
	graph.links.push_back(MaterialLink{.id = 10, .fromNode = 2, .fromPin = 0, .toNode = 1, .toPin = 0});
	graph.links.push_back(MaterialLink{.id = 11, .fromNode = 3, .fromPin = 0, .toNode = 2, .toPin = 0});
	graph.links.push_back(MaterialLink{.id = 12, .fromNode = 2, .fromPin = 0, .toNode = 3, .toPin = 0});

	std::string error;
	CHECK(GenerateMaterialShader(graph, error).empty());
	CHECK(error.find("itself") != std::string::npos);
}

TEST_CASE("A node feeding two inputs is emitted once")
{
	MaterialGraph graph;
	graph.nodes.push_back(MaterialNode{.id = 1, .type = MaterialNodeType::Output});
	graph.nodes.push_back(MaterialNode{.id = 2, .type = MaterialNodeType::ConstantColor});
	graph.links.push_back(MaterialLink{.id = 10, .fromNode = 2, .fromPin = 0, .toNode = 1, .toPin = 0});
	graph.links.push_back(MaterialLink{.id = 11, .fromNode = 2, .fromPin = 0, .toNode = 1, .toPin = 3});

	std::string error;
	const std::string shader = GenerateMaterialShader(graph, error);
	CHECK(error.empty());
	std::size_t declarations = 0;
	for (std::size_t at = shader.find("const float4 n2 ="); at != std::string::npos; at = shader.find("const float4 n2 =", at + 1))
	{
		++declarations;
	}
	CHECK(declarations == 1);
}

TEST_CASE("A graph survives a round trip through its file form")
{
	MaterialGraph graph = MakeDefaultMaterialGraph();
	graph.nodes.push_back(MaterialNode{.id = 7, .type = MaterialNodeType::TextureSample, .x = 12.5f, .y = -3.25f});
	graph.nodes.back().slot = MaterialTextureSlot::Emissive;
	graph.links.push_back(MaterialLink{.id = 8, .fromNode = 7, .fromPin = 0, .toNode = 1, .toPin = 3});

	const MaterialGraph parsed = ParseMaterialGraph(SerializeMaterialGraph(graph));
	REQUIRE(parsed.nodes.size() == graph.nodes.size());
	REQUIRE(parsed.links.size() == graph.links.size());
	CHECK(parsed.nextId == graph.nextId);
	const MaterialNode* tex = parsed.Find(7);
	REQUIRE(tex != nullptr);
	CHECK(tex->type == MaterialNodeType::TextureSample);
	CHECK(tex->slot == MaterialTextureSlot::Emissive);
	CHECK(tex->x == doctest::Approx(12.5f));
	CHECK(tex->y == doctest::Approx(-3.25f));
	const MaterialLink* link = parsed.LinkInto(1, 3);
	REQUIRE(link != nullptr);
	CHECK(link->fromNode == 7);
}

// Writes the generated shader out so the build can run it through slangc. The generator can
// produce a plausible-looking string that the compiler rejects, and only the compiler knows.
TEST_CASE("Generated shader is written for compilation check")
{
	std::string error;
	MaterialGraph graph = MakeDefaultMaterialGraph();
	graph.nodes.push_back(MaterialNode{.id = 7, .type = MaterialNodeType::TextureSample});
	graph.nodes.push_back(MaterialNode{.id = 8, .type = MaterialNodeType::Fresnel});
	graph.nodes.push_back(MaterialNode{.id = 9, .type = MaterialNodeType::Multiply});
	graph.nodes.push_back(MaterialNode{.id = 10, .type = MaterialNodeType::Uv});
	graph.nodes.push_back(MaterialNode{.id = 11, .type = MaterialNodeType::Time});
	graph.nodes.push_back(MaterialNode{.id = 12, .type = MaterialNodeType::Lerp});
	graph.links.push_back(MaterialLink{.id = 20, .fromNode = 7, .fromPin = 0, .toNode = 9, .toPin = 0});
	graph.links.push_back(MaterialLink{.id = 21, .fromNode = 8, .fromPin = 0, .toNode = 9, .toPin = 1});
	graph.links.push_back(MaterialLink{.id = 22, .fromNode = 9, .fromPin = 0, .toNode = 12, .toPin = 0});
	graph.links.push_back(MaterialLink{.id = 23, .fromNode = 10, .fromPin = 0, .toNode = 12, .toPin = 1});
	graph.links.push_back(MaterialLink{.id = 24, .fromNode = 11, .fromPin = 0, .toNode = 12, .toPin = 2});
	graph.links.push_back(MaterialLink{.id = 25, .fromNode = 12, .fromPin = 0, .toNode = 1, .toPin = 3});

	const std::string shader = GenerateMaterialShader(graph, error);
	REQUIRE(error.empty());
	if (const char* out = std::getenv("AETHER_DUMP_GENERATED_SHADER"); out != nullptr)
	{
		std::ofstream(out) << shader;
	}
	CHECK(shader.find("n12") != std::string::npos);
}
