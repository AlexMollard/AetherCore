#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aether::editor
{
	// A node graph that generates a material's fragment shader.
	//
	// Every value in the graph is a float4, and nodes swizzle what they need out of it. A real
	// type system (float/float2/float3/float4 with promotion rules) is the right end state, but
	// it is also most of the complexity of a shader graph - carrying one width first keeps the
	// generator small enough to be tested exhaustively, and the widening rules can be added
	// underneath it without changing any node.
	enum class MaterialNodeType : std::uint8_t
	{
		Output,
		ConstantColor,
		ConstantFloat,
		TextureSample,
		Uv,
		Time,
		Fresnel,
		Multiply,
		Add,
		Lerp,
	};

	// Which of the material's texture slots a TextureSample node reads. Reusing the slots the
	// material already carries means a graph needs no texture binding path of its own.
	enum class MaterialTextureSlot : std::uint8_t
	{
		Albedo,
		Normal,
		MetallicRoughness,
		Occlusion,
		Emissive,
	};

	struct MaterialNode
	{
		int id = 0;
		MaterialNodeType type = MaterialNodeType::ConstantFloat;
		float x = 0.0f;
		float y = 0.0f;
		float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		MaterialTextureSlot slot = MaterialTextureSlot::Albedo;
	};

	struct MaterialLink
	{
		int id = 0;
		int fromNode = 0;
		int fromPin = 0;
		int toNode = 0;
		int toPin = 0;
	};

	struct MaterialGraph
	{
		std::vector<MaterialNode> nodes;
		std::vector<MaterialLink> links;
		int nextId = 1;

		[[nodiscard]] const MaterialNode* Find(int nodeId) const;
		[[nodiscard]] MaterialNode* Find(int nodeId);
		// The link feeding an input, or nullptr when that input is unconnected.
		[[nodiscard]] const MaterialLink* LinkInto(int nodeId, int pin) const;
	};

	[[nodiscard]] const char* MaterialNodeTypeName(MaterialNodeType type);
	[[nodiscard]] int MaterialNodeInputCount(MaterialNodeType type);
	[[nodiscard]] const char* MaterialNodeInputName(MaterialNodeType type, int pin);

	// A graph with an Output node and a mid-grey base colour, so a new graph compiles.
	[[nodiscard]] MaterialGraph MakeDefaultMaterialGraph();

	[[nodiscard]] std::string SerializeMaterialGraph(const MaterialGraph& graph);
	[[nodiscard]] MaterialGraph ParseMaterialGraph(const std::string& text);

	// Emits a complete Slang fragment shader. `error` is set and the result is empty when the
	// graph cannot be generated - a cycle, or no Output node.
	[[nodiscard]] std::string GenerateMaterialShader(const MaterialGraph& graph, std::string& error);
} // namespace aether::editor
