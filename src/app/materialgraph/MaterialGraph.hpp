#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aether::editor
{
	// A node graph that generates a material's fragment shader.
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

	// How wide a value is.
	//
	// `Any` is not a value: it marks a pin that takes whatever it is given, and a node whose
	// output width follows its inputs - Multiply of two float3s is a float3, of two floats is
	// a float. Resolving those is what MaterialNodeOutputType does.
	enum class MaterialValueType : std::uint8_t
	{
		Any,
		Float,
		Float2,
		Float3,
		Float4,
	};

	[[nodiscard]] int MaterialValueComponents(MaterialValueType type);
	// "float3" - the name used in the generated shader, and in the UI.
	[[nodiscard]] const char* MaterialValueTypeName(MaterialValueType type);

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

	// The width a pin declares. `Any` means the node adapts to whatever arrives.
	[[nodiscard]] MaterialValueType MaterialNodeInputType(MaterialNodeType type, int pin);

	// The width a node actually produces, which for the arithmetic nodes depends on what is
	// plugged into them. Never returns Any: an unconnected adaptive node settles on Float.
	[[nodiscard]] MaterialValueType MaterialNodeOutputType(const MaterialGraph& graph, int nodeId);

	// Whether one pin can drive another, and what happens to the value if it does.
	enum class MaterialConnection : std::uint8_t
	{
		Exact,     // same width
		Broadcast, // a single float filling every component
		Truncate,  // a wider value with its extra components dropped
		Refused,   // no sensible conversion exists
	};

	[[nodiscard]] MaterialConnection MaterialCanConnect(MaterialValueType from, MaterialValueType to);
	// Why a connection was refused, for the editor to show. Empty when it was not.
	[[nodiscard]] std::string MaterialConnectionRefusal(MaterialValueType from, MaterialValueType to);

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
