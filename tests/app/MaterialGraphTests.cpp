#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
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

	const std::optional<MaterialGraph> read = ParseMaterialGraph(SerializeMaterialGraph(graph));
	REQUIRE(read.has_value());
	const MaterialGraph& parsed = *read;
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

	// The later nodes, including the ones that need the TBN and the noise helper, plus a
	// Panner driving a texture's UV - which is the reason TextureSample has a UV pin at all.
	graph.nodes.push_back(MaterialNode{.id = 13, .type = MaterialNodeType::Panner});
	graph.nodes.back().value[0] = 0.1f;
	graph.nodes.back().value[1] = -0.05f;
	graph.nodes.push_back(MaterialNode{.id = 14, .type = MaterialNodeType::Noise});
	graph.nodes.back().value[0] = 8.0f;
	graph.nodes.push_back(MaterialNode{.id = 15, .type = MaterialNodeType::Step});
	graph.nodes.push_back(MaterialNode{.id = 16, .type = MaterialNodeType::NormalMap});
	graph.nodes.back().slot = MaterialTextureSlot::Normal;
	graph.links.push_back(MaterialLink{.id = 26, .fromNode = 13, .fromPin = 0, .toNode = 7, .toPin = 0});
	graph.links.push_back(MaterialLink{.id = 27, .fromNode = 13, .fromPin = 0, .toNode = 14, .toPin = 0});
	graph.links.push_back(MaterialLink{.id = 28, .fromNode = 14, .fromPin = 0, .toNode = 15, .toPin = 1});
	graph.links.push_back(MaterialLink{.id = 29, .fromNode = 15, .fromPin = 0, .toNode = 1, .toPin = 2});
	graph.links.push_back(MaterialLink{.id = 30, .fromNode = 16, .fromPin = 0, .toNode = 1, .toPin = 4});

	const std::string shader = GenerateMaterialShader(graph, error);
	REQUIRE(error.empty());
	if (const char* out = std::getenv("AETHER_DUMP_GENERATED_SHADER"); out != nullptr)
	{
		std::ofstream(out) << shader;
	}
	CHECK(shader.find("n12") != std::string::npos);
	CHECK(shader.find("GraphValueNoise") != std::string::npos);
	CHECK(shader.find("graphTBN") != std::string::npos);
}

// The add menu is built from this, so a node type that never reaches it is a node nobody can
// place. Output is the exception: every graph already has one and a second would be ambiguous.
TEST_CASE("Every node type except Output is reachable from the add menu")
{
	const std::span<const MaterialNodeType> addable = MaterialAddableNodeTypes();
	for (int i = 0; i <= static_cast<int>(MaterialNodeType::ViewDirection); ++i)
	{
		const auto type = static_cast<MaterialNodeType>(i);
		const bool present = std::ranges::find(addable, type) != addable.end();
		CHECK(present == (type != MaterialNodeType::Output));
	}
	CHECK(std::ranges::find(addable, MaterialNodeType::Output) == addable.end());
}

TEST_CASE("The add menu is grouped, so a category never appears twice")
{
	std::vector<MaterialNodeCategory> runs;
	for (const MaterialNodeType type: MaterialAddableNodeTypes())
	{
		const MaterialNodeCategory category = MaterialNodeCategoryOf(type);
		if (runs.empty() || runs.back() != category)
		{
			runs.push_back(category);
		}
	}
	std::vector<MaterialNodeCategory> unique = runs;
	std::ranges::sort(unique, [](auto a, auto b) { return static_cast<int>(a) < static_cast<int>(b); });
	CHECK(std::ranges::unique(unique).begin() == unique.end());
}

TEST_CASE("Every node type has a name and a description")
{
	for (int i = 0; i <= static_cast<int>(MaterialNodeType::ViewDirection); ++i)
	{
		const auto type = static_cast<MaterialNodeType>(i);
		CHECK(std::string_view(MaterialNodeTypeName(type)).size() > 0);
		CHECK(std::string_view(MaterialNodeDescription(type)).size() > 0);
	}
}

// A Remap that has not been dialled in must not change what it is given, or dropping one
// silently darkens the graph.
TEST_CASE("A freshly made Remap is the identity")
{
	const MaterialNode node = MakeMaterialNode(1, MaterialNodeType::Remap);
	CHECK(node.value[0] == doctest::Approx(0.0f));
	CHECK(node.value[1] == doctest::Approx(1.0f));
	CHECK(node.value[2] == doctest::Approx(0.0f));
	CHECK(node.value[3] == doctest::Approx(1.0f));
}

TEST_CASE("A Channel selection survives a round trip through the file form")
{
	MaterialGraph graph = MakeDefaultMaterialGraph();
	MaterialNode channel = MakeMaterialNode(9, MaterialNodeType::Channel);
	channel.channel = 2;
	graph.nodes.push_back(channel);

	const std::optional<MaterialGraph> read = ParseMaterialGraph(SerializeMaterialGraph(graph));
	REQUIRE(read.has_value());
	const MaterialGraph& parsed = *read;
	const MaterialNode* found = parsed.Find(9);
	REQUIRE(found != nullptr);
	CHECK(found->channel == 2);
}

// Every node the menu offers has to reach the generator. A missing case emitted a black
// float4 that looked like a wiring mistake rather than a gap in the switch.
TEST_CASE("Every addable node type generates an expression")
{
	for (const MaterialNodeType type: MaterialAddableNodeTypes())
	{
		MaterialGraph graph;
		graph.nodes.push_back(MaterialNode{.id = 1, .type = MaterialNodeType::Output});
		graph.nodes.push_back(MakeMaterialNode(2, type));
		graph.links.push_back(MaterialLink{.id = 3, .fromNode = 2, .fromPin = 0, .toNode = 1, .toPin = 0});

		std::string error;
		const std::string shader = GenerateMaterialShader(graph, error);
		CAPTURE(MaterialNodeTypeName(type));
		REQUIRE(error.empty());
		const std::size_t at = shader.find("const float4 n2 = ");
		REQUIRE(at != std::string::npos);
		// The all-zero placeholder is what an unhandled type falls through to.
		CHECK(shader.compare(at, std::strlen("const float4 n2 = float4(0, 0, 0, 1);"), "const float4 n2 = float4(0, 0, 0, 1);") != 0);
	}
}

// Written out for the build's slangc pass, exactly as the smaller graph above is: the maths
// nodes are where a plausible-looking expression is most likely to be rejected.
TEST_CASE("A graph using every node compiles to a shader")
{
	MaterialGraph graph;
	graph.nodes.push_back(MaterialNode{.id = 1, .type = MaterialNodeType::Output});
	int id = 2;
	int previous = 0;
	for (const MaterialNodeType type: MaterialAddableNodeTypes())
	{
		MaterialNode node = MakeMaterialNode(id++, type);
		graph.nodes.push_back(node);
		// Chained through the first input so every node is actually reached from the Output
		// rather than emitted in isolation.
		if (previous != 0)
		{
			graph.links.push_back(MaterialLink{.id = id + 1000, .fromNode = previous, .fromPin = 0, .toNode = node.id, .toPin = 0});
		}
		previous = node.id;
	}
	graph.links.push_back(MaterialLink{.id = 999, .fromNode = previous, .fromPin = 0, .toNode = 1, .toPin = 0});
	graph.nextId = id;

	std::string error;
	const std::string shader = GenerateMaterialShader(graph, error);
	REQUIRE(error.empty());
	if (const char* out = std::getenv("AETHER_DUMP_GENERATED_SHADER_ALL"); out != nullptr)
	{
		std::ofstream(out) << shader;
	}
	CHECK(shader.find("GraphSafeDivide") != std::string::npos);
	CHECK(shader.find("GraphRemap") != std::string::npos);
}

// The graph lives INSIDE the material file now. Its tables have to survive sitting after a
// material's own, which is the arrangement every real file has.
TEST_CASE("A graph parses out of a full material file")
{
	const MaterialGraph graph = MakeDefaultMaterialGraph();
	const std::string file = "[material]\nbasecolorfactor = [ 1, 1, 1, 1 ]\nroughnessfactor = 0.5\n\n[textures]\nalbedo = 'a.png'\n\n"
	        + SerializeMaterialGraph(graph);

	const std::optional<MaterialGraph> parsed = ParseMaterialGraph(file);
	REQUIRE(parsed.has_value());
	CHECK(parsed->nodes.size() == graph.nodes.size());
	CHECK(parsed->links.size() == graph.links.size());
}

// Telling a hand-authored material from a generated one is what decides whether the window
// shows a canvas at all, so "no graph" must not come back as an empty default graph.
TEST_CASE("A material with no graph reports no graph")
{
	CHECK(!ParseMaterialGraph("[material]\nroughnessfactor = 0.5\n").has_value());
	CHECK(!ParseMaterialGraph("").has_value());
}

// "[graphics]" must not be mistaken for the graph block by a prefix test.
TEST_CASE("A section that merely starts with graph is not a graph")
{
	CHECK(!ParseMaterialGraph("[graphics]\nquality = 2\n").has_value());
}
