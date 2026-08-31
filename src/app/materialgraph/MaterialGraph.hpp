#pragma once

#include <cstdint>
#include <optional>
#include <span>
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
		NormalMap,
		Panner,
		Noise,
		Step,
		// Appended, never reordered: a graph file stores the type as its integer, so moving
		// an entry silently rewrites every saved graph into different nodes.
		Subtract,
		Divide,
		OneMinus,
		Power,
		Saturate,
		Dot,
		Smoothstep,
		Sine,
		Remap,
		Channel,
		WorldPosition,
		VertexColor,
		ViewDirection,
	};

	// What a node is FOR, which is how the add menu is grouped. A flat list stopped being
	// findable somewhere around a dozen entries.
	enum class MaterialNodeCategory : std::uint8_t
	{
		Input,
		Texture,
		Math,
		Output,
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
		// Which component a Channel node reads: 0=r, 1=g, 2=b, 3=a.
		int channel = 0;
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
	[[nodiscard]] const char* MaterialNodeDescription(MaterialNodeType type);
	[[nodiscard]] MaterialNodeCategory MaterialNodeCategoryOf(MaterialNodeType type);
	[[nodiscard]] const char* MaterialNodeCategoryName(MaterialNodeCategory category);
	[[nodiscard]] int MaterialNodeInputCount(MaterialNodeType type);
	[[nodiscard]] const char* MaterialNodeInputName(MaterialNodeType type, int pin);

	// Every type the add menu offers, in menu order. Derived from the enum rather than
	// hand-listed at the call site, so a new node cannot be added and left unreachable.
	[[nodiscard]] std::span<const MaterialNodeType> MaterialAddableNodeTypes();

	// A node carrying the defaults that make it show something on first drop - an all-ones
	// Panner scrolls a UV per second, which reads as broken rather than as a default.
	[[nodiscard]] MaterialNode MakeMaterialNode(int id, MaterialNodeType type);

	// A graph with an Output node and a mid-grey base colour, so a new graph compiles.
	[[nodiscard]] MaterialGraph MakeDefaultMaterialGraph();

	// The graph as the `[graph]` tables of a material file. A material is ONE file: the graph
	// that generates its shader is part of it, not a sidecar that has to be renamed, copied
	// and deleted alongside it.
	[[nodiscard]] std::string SerializeMaterialGraph(const MaterialGraph& graph);

	// Reads those tables back out of a material file. Empty when the material carries no
	// graph, which is how a hand-authored material is told apart from a generated one.
	[[nodiscard]] std::optional<MaterialGraph> ParseMaterialGraph(const std::string& materialToml);

	// Emits a complete Slang fragment shader. `error` is set and the result is empty when the
	// graph cannot be generated - a cycle, or no Output node.
	[[nodiscard]] std::string GenerateMaterialShader(const MaterialGraph& graph, std::string& error);
} // namespace aether::editor
