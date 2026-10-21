#include <doctest/doctest.h>
#include <limits>
#include "materialgraph/MaterialGraph.hpp"

using namespace aether::editor;

TEST_CASE("Web graph normal maps sample before divergent texture presence selection")
{
	auto graph = MakeDefaultMaterialGraph();
	graph.nodes.push_back(MakeMaterialNode(100, MaterialNodeType::NormalMap));
	graph.nodes.push_back(MakeMaterialNode(101, MaterialNodeType::Uv));
	graph.links.push_back({102, 101, 0, 100, 0});
	graph.links.push_back({103, 100, 0, 1, 4});
	std::string error;
	const auto shader = GenerateWebMaterialSurface(graph, error);
	REQUIRE(error.empty());
	const auto sample = shader.find("graph_sample_100 = GraphSampleTexture");
	const auto select = shader.find("GraphTexturePresent(input.textureMask");
	REQUIRE(sample != std::string::npos);
	REQUIRE(select != std::string::npos);
	CHECK(sample < select);
	CHECK(shader.find("? normalize(mul(GraphSampleTexture") == std::string::npos);
}

TEST_CASE("Native graph forward and prepass share the same evaluated surface")
{
	std::string error;
	const auto shader = GenerateMaterialShader(MakeDefaultMaterialGraph(), error);
	REQUIRE(error.empty());
	CHECK(shader.find("graphPrepassMain") != std::string::npos);
	const std::string evaluation = "EvaluateNativeGraphSurface(input,";
	const auto first = shader.find(evaluation);
	REQUIRE(first != std::string::npos);
	CHECK(shader.find(evaluation, first + evaluation.size()) != std::string::npos);
}

TEST_CASE("Web material source rejects parser repairs instead of silently changing the graph")
{
	std::string error;
	CHECK(CompileWebMaterialSurface("[graph.node.0]\nid=1\ntype=999\n", error).empty());
	CHECK(error.find("NODE") != std::string::npos);
	CHECK(CompileWebMaterialSurface("[graph.node.0]\nid=1\ntype=0\nslot=7\n", error).empty());
	CHECK(error.find("NODE") != std::string::npos);
	CHECK(CompileWebMaterialSurface("[graph.node.0]\nid=1\ntype=0\n[graph.link.0]\nfrom_node=nan\n", error).empty());
	CHECK(error.find("NODE") != std::string::npos);
}

TEST_CASE("Web material surfaces reuse graph nodes without native addresses")
{
	std::string error;
	const auto shader = GenerateWebMaterialSurface(MakeDefaultMaterialGraph(), error);
	REQUIRE(error.empty());
	CHECK(shader.find("EvaluateGraphSurface") != std::string::npos);
	CHECK(shader.find("0.6") != std::string::npos);
	CHECK(shader.find("DevicePtr") == std::string::npos);
	CHECK(shader.find("fc->") == std::string::npos);
	CHECK(shader.find("g_textures[") == std::string::npos);
}

TEST_CASE("Web graph portability rejects nonfinite nodes and oversized graphs")
{
	std::string error;
	auto graph = MakeDefaultMaterialGraph();
	graph.nodes[1].value[0] = std::numeric_limits<float>::quiet_NaN();
	CHECK(GenerateWebMaterialSurface(graph, error).empty());
	CHECK(error.find("finite") != std::string::npos);
	graph = MakeDefaultMaterialGraph();
	graph.nodes.resize(257);
	CHECK(GenerateWebMaterialSurface(graph, error).empty());
	CHECK(error.find("budget") != std::string::npos);
}
