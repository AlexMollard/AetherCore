#include <doctest/doctest.h>
#include <limits>
#include "materialgraph/MaterialGraph.hpp"

using namespace aether::editor;

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
