#include "materialgraph/MaterialGraph.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils/TextIni.hpp"

namespace aether::editor
{
	namespace
	{
		struct NodeSpec
		{
			const char* name;
			MaterialNodeCategory category;
			int inputs;
			const char* inputNames[3];
			const char* description;
		};

		// Indexed by MaterialNodeType, so the order here follows the enum exactly.
		constexpr NodeSpec kSpecs[] = {
		        {"Output", MaterialNodeCategory::Output, 5, {"", "", ""}, "What the surface is made of."},
		        {"Colour", MaterialNodeCategory::Input, 0, {"", "", ""}, "A constant colour."},
		        {"Float", MaterialNodeCategory::Input, 0, {"", "", ""}, "A constant number, on every channel."},
		        {"Texture", MaterialNodeCategory::Texture, 1, {"UV", "", ""}, "Samples one of the material's texture slots."},
		        {"UV", MaterialNodeCategory::Input, 0, {"", "", ""}, "The mesh's texture coordinates."},
		        {"Time", MaterialNodeCategory::Input, 0, {"", "", ""}, "Seconds since the engine started."},
		        {"Fresnel", MaterialNodeCategory::Input, 0, {"", "", ""}, "Rim term: bright where the surface faces away from the camera."},
		        {"Multiply", MaterialNodeCategory::Math, 2, {"A", "B", ""}, "A * B."},
		        {"Add", MaterialNodeCategory::Math, 2, {"A", "B", ""}, "A + B."},
		        {"Lerp", MaterialNodeCategory::Math, 3, {"A", "B", "T"}, "Blends from A to B by T."},
		        {"Normal map", MaterialNodeCategory::Texture, 1, {"UV", "", ""}, "Unpacks a tangent-space normal map into world space."},
		        {"Panner", MaterialNodeCategory::Math, 1, {"UV", "", ""}, "Scrolls a UV over time."},
		        {"Noise", MaterialNodeCategory::Math, 1, {"UV", "", ""}, "Value noise over a UV."},
		        {"Step", MaterialNodeCategory::Math, 2, {"Edge", "X", ""}, "0 below the edge, 1 above it. A hard cut."},
		        {"Subtract", MaterialNodeCategory::Math, 2, {"A", "B", ""}, "A - B."},
		        {"Divide", MaterialNodeCategory::Math, 2, {"A", "B", ""}, "A / B, guarded against dividing by zero."},
		        {"One minus", MaterialNodeCategory::Math, 1, {"X", "", ""}, "1 - X. Inverts a mask."},
		        {"Power", MaterialNodeCategory::Math, 2, {"X", "Exp", ""}, "X raised to Exp. Sharpens a gradient."},
		        {"Saturate", MaterialNodeCategory::Math, 1, {"X", "", ""}, "Clamps to 0..1."},
		        {"Dot", MaterialNodeCategory::Math, 2, {"A", "B", ""}, "Dot product of the xyz parts, on every channel."},
		        {"Smoothstep", MaterialNodeCategory::Math, 3, {"Edge 0", "Edge 1", "X"}, "A soft Step: eased from 0 to 1 between the edges."},
		        {"Sine", MaterialNodeCategory::Math, 1, {"X", "", ""}, "sin(X * frequency), for anything that pulses."},
		        {"Remap", MaterialNodeCategory::Math, 1, {"X", "", ""}, "Rescales X from one range to another."},
		        {"Channel", MaterialNodeCategory::Math, 1, {"X", "", ""}, "Picks one channel and broadcasts it."},
		        {"World position", MaterialNodeCategory::Input, 0, {"", "", ""}, "The shaded point, in world space."},
		        {"Vertex colour", MaterialNodeCategory::Input, 0, {"", "", ""}, "The mesh's per-vertex colour."},
		        {"View direction", MaterialNodeCategory::Input, 0, {"", "", ""}, "Unit vector from the surface towards the camera."},
		};
		static_assert(std::size(kSpecs) == static_cast<std::size_t>(MaterialNodeType::ViewDirection) + 1,
		        "Every MaterialNodeType needs a spec; the table is indexed by the enum.");

		const NodeSpec& SpecOf(MaterialNodeType type)
		{
			return kSpecs[static_cast<std::size_t>(type)];
		}

		const char* kOutputPinNames[5] = {"Base colour", "Metallic", "Roughness", "Emissive", "Normal"};

		const char* SlotExpression(MaterialTextureSlot slot)
		{
			switch (slot)
			{
				case MaterialTextureSlot::Normal:
					return "mat.normalSlot";
				case MaterialTextureSlot::MetallicRoughness:
					return "mat.metallicRoughnessSlot";
				case MaterialTextureSlot::Occlusion:
					return "mat.occlusionSlot";
				case MaterialTextureSlot::Emissive:
					return "mat.emissiveSlot";
				case MaterialTextureSlot::Albedo:
				default:
					return "mat.albedoSlot";
			}
		}
	} // namespace

	const char* MaterialNodeTypeName(MaterialNodeType type)
	{
		return SpecOf(type).name;
	}

	const char* MaterialNodeDescription(MaterialNodeType type)
	{
		return SpecOf(type).description;
	}

	MaterialNodeCategory MaterialNodeCategoryOf(MaterialNodeType type)
	{
		return SpecOf(type).category;
	}

	const char* MaterialNodeCategoryName(MaterialNodeCategory category)
	{
		switch (category)
		{
			case MaterialNodeCategory::Input:
				return "Input";
			case MaterialNodeCategory::Texture:
				return "Texture";
			case MaterialNodeCategory::Math:
				return "Math";
			case MaterialNodeCategory::Output:
			default:
				return "Output";
		}
	}

	std::span<const MaterialNodeType> MaterialAddableNodeTypes()
	{
		// Built once from the enum and ordered by category, so adding a type to the enum puts
		// it in the menu without a second list to remember.
		static const std::vector<MaterialNodeType> kAddable = []
		{
			std::vector<MaterialNodeType> out;
			for (const MaterialNodeCategory category: {MaterialNodeCategory::Input, MaterialNodeCategory::Texture, MaterialNodeCategory::Math})
			{
				for (std::size_t i = 0; i < std::size(kSpecs); ++i)
				{
					const auto type = static_cast<MaterialNodeType>(i);
					if (MaterialNodeCategoryOf(type) == category)
					{
						out.push_back(type);
					}
				}
			}
			return out;
		}();
		return kAddable;
	}

	MaterialNode MakeMaterialNode(int id, MaterialNodeType type)
	{
		MaterialNode node{.id = id, .type = type};
		switch (type)
		{
			case MaterialNodeType::Panner:
				node.value[0] = 0.1f;
				node.value[1] = 0.0f;
				break;
			case MaterialNodeType::Noise:
				node.value[0] = 8.0f;
				break;
			case MaterialNodeType::NormalMap:
				node.slot = MaterialTextureSlot::Normal;
				break;
			case MaterialNodeType::Remap:
				// Identity, so dropping one changes nothing until it is dialled in.
				node.value[0] = 0.0f;
				node.value[1] = 1.0f;
				node.value[2] = 0.0f;
				node.value[3] = 1.0f;
				break;
			default:
				break;
		}
		return node;
	}

	int MaterialNodeInputCount(MaterialNodeType type)
	{
		return SpecOf(type).inputs;
	}

	const char* MaterialNodeInputName(MaterialNodeType type, int pin)
	{
		if (type == MaterialNodeType::Output)
		{
			return (pin >= 0 && pin < 5) ? kOutputPinNames[pin] : "";
		}
		const NodeSpec& spec = SpecOf(type);
		return (pin >= 0 && pin < spec.inputs) ? spec.inputNames[pin] : "";
	}

	const MaterialNode* MaterialGraph::Find(int nodeId) const
	{
		const auto it = std::ranges::find(nodes, nodeId, &MaterialNode::id);
		return it == nodes.end() ? nullptr : &*it;
	}

	MaterialNode* MaterialGraph::Find(int nodeId)
	{
		const auto it = std::ranges::find(nodes, nodeId, &MaterialNode::id);
		return it == nodes.end() ? nullptr : &*it;
	}

	const MaterialLink* MaterialGraph::LinkInto(int nodeId, int pin) const
	{
		const auto it = std::ranges::find_if(links, [&](const MaterialLink& l) { return l.toNode == nodeId && l.toPin == pin; });
		return it == links.end() ? nullptr : &*it;
	}

	MaterialGraph MakeDefaultMaterialGraph()
	{
		MaterialGraph graph;
		MaterialNode out{.id = 1, .type = MaterialNodeType::Output, .x = 420.0f, .y = 140.0f};
		MaterialNode colour{.id = 2, .type = MaterialNodeType::ConstantColor, .x = 120.0f, .y = 120.0f};
		colour.value[0] = 0.6f;
		colour.value[1] = 0.6f;
		colour.value[2] = 0.6f;
		colour.value[3] = 1.0f;
		graph.nodes = {out, colour};
		graph.links = {MaterialLink{.id = 3, .fromNode = 2, .fromPin = 0, .toNode = 1, .toPin = 0}};
		graph.nextId = 4;
		return graph;
	}

	std::string SerializeMaterialGraph(const MaterialGraph& graph)
	{
		std::string out = "# Node graph for this material's shader. Edited in the editor's Material window.\n";
		out += "[graph]\n";
		out += std::format("next_id = {}\n", graph.nextId);
		// Indexed sub-tables rather than [[graph.node]] arrays of tables: text::ParseToml
		// descends into child TABLES but reports an array as one value, so an array of tables
		// reads back as nothing at all.
		int index = 0;
		for (const MaterialNode& node: graph.nodes)
		{
			out += std::format("\n[graph.node.{}]\n", index++);
			out += std::format("id = {}\n", node.id);
			out += std::format("type = {}\n", static_cast<int>(node.type));
			out += std::format("x = {}\n", node.x);
			out += std::format("y = {}\n", node.y);
			out += std::format("value = [ {}, {}, {}, {} ]\n", node.value[0], node.value[1], node.value[2], node.value[3]);
			out += std::format("slot = {}\n", static_cast<int>(node.slot));
			out += std::format("channel = {}\n", node.channel);
		}
		index = 0;
		for (const MaterialLink& link: graph.links)
		{
			out += std::format("\n[graph.link.{}]\n", index++);
			out += std::format("id = {}\n", link.id);
			out += std::format("from_node = {}\n", link.fromNode);
			out += std::format("from_pin = {}\n", link.fromPin);
			out += std::format("to_node = {}\n", link.toNode);
			out += std::format("to_pin = {}\n", link.toPin);
		}
		return out;
	}

	std::optional<MaterialGraph> ParseMaterialGraph(const std::string& materialToml)
	{
		MaterialGraph graph;
		MaterialNode node;
		MaterialLink link;
		std::string current;
		bool inNode = false;
		bool inLink = false;
		bool sawGraph = false;
		const auto flush = [&]()
		{
			if (inNode)
			{
				graph.nodes.push_back(node);
			}
			if (inLink)
			{
				graph.links.push_back(link);
			}
			inNode = false;
			inLink = false;
		};

		text::ParseToml(materialToml,
		        [&](const text::IniEntry& entry)
		        {
			        // Each element has its own section - "graph.node.0", "graph.link.3" - so a
			        // change of section is exactly the boundary between two of them.
			        if (entry.section != current)
			        {
				        flush();
				        current = entry.section;
				        if (current.starts_with("graph.node."))
				        {
					        node = MaterialNode{};
					        inNode = true;
					        sawGraph = true;
				        }
				        else if (current.starts_with("graph.link."))
				        {
					        link = MaterialLink{};
					        inLink = true;
					        sawGraph = true;
				        }
			        }

			        const auto asInt = [&](int fallback) { return static_cast<int>(text::ParseFloat(entry.value).value_or(static_cast<float>(fallback))); };
			        if (entry.fullKey == "graph.next_id")
			        {
				        graph.nextId = asInt(1);
				        sawGraph = true;
			        }
			        else if (inNode)
			        {
				        if (entry.key == "id") { node.id = asInt(0); }
				        else if (entry.key == "type") { node.type = static_cast<MaterialNodeType>(asInt(0)); }
				        else if (entry.key == "x") { node.x = text::ParseFloat(entry.value).value_or(0.0f); }
				        else if (entry.key == "y") { node.y = text::ParseFloat(entry.value).value_or(0.0f); }
				        else if (entry.key == "slot") { node.slot = static_cast<MaterialTextureSlot>(asInt(0)); }
				        else if (entry.key == "channel") { node.channel = std::clamp(asInt(0), 0, 3); }
				        else if (entry.key == "value")
				        {
					        if (const auto parsed = text::ParseFloatArray<4>(entry.value))
					        {
						        for (int i = 0; i < 4; ++i)
						        {
							        node.value[i] = (*parsed)[i];
						        }
					        }
				        }
			        }
			        else if (inLink)
			        {
				        if (entry.key == "id") { link.id = asInt(0); }
				        else if (entry.key == "from_node") { link.fromNode = asInt(0); }
				        else if (entry.key == "from_pin") { link.fromPin = asInt(0); }
				        else if (entry.key == "to_node") { link.toNode = asInt(0); }
				        else if (entry.key == "to_pin") { link.toPin = asInt(0); }
			        }
		        });
		flush();
		if (!sawGraph || graph.nodes.empty())
		{
			return std::nullopt;
		}
		return graph;
	}

	namespace
	{
		// Emits one float4 temporary per node, memoised, so a node feeding three inputs is
		// evaluated once. Returns the variable name holding that node's value.
		struct Emitter
		{
			const MaterialGraph& graph;
			std::string& body;
			std::unordered_map<int, std::string> done;
			std::unordered_set<int> onStack;
			std::string error;

			std::string Input(int nodeId, int pin, const char* fallback)
			{
				if (const MaterialLink* link = graph.LinkInto(nodeId, pin); link != nullptr)
				{
					return Visit(link->fromNode);
				}
				return fallback;
			}

			std::string Visit(int nodeId)
			{
				if (const auto it = done.find(nodeId); it != done.end())
				{
					return it->second;
				}
				if (!onStack.insert(nodeId).second)
				{
					// A cycle would otherwise recurse until the stack ran out.
					error = "This graph feeds a node back into itself.";
					return "float4(0, 0, 0, 1)";
				}

				const MaterialNode* node = graph.Find(nodeId);
				if (node == nullptr)
				{
					onStack.erase(nodeId);
					return "float4(0, 0, 0, 1)";
				}

				const std::string var = std::format("n{}", nodeId);
				std::string expr;
				switch (node->type)
				{
					case MaterialNodeType::ConstantColor:
					case MaterialNodeType::ConstantFloat:
						expr = std::format("float4({}, {}, {}, {})", node->value[0], node->value[1], node->value[2], node->value[3]);
						break;
					case MaterialNodeType::Uv:
						expr = "float4(input.uv, 0.0f, 1.0f)";
						break;
					case MaterialNodeType::Time:
						expr = "float4(fc->elapsedTime, fc->elapsedTime, fc->elapsedTime, 1.0f)";
						break;
					case MaterialNodeType::Fresnel:
						// Schlick with a fixed power: the rim term everyone reaches for first.
						expr = "float4(pow(1.0f - saturate(dot(normalize(input.worldNormal), normalize(fc->cameraWorldPos.xyz - input.worldPos))), 5.0f).xxx, 1.0f)";
						break;
					case MaterialNodeType::TextureSample:
					{
						// Sampling at the node's own UV rather than the interpolated one is
						// the whole point of having a Panner or any other UV maths upstream.
						const std::string uv = Input(nodeId, 0, "float4(input.uv, 0.0f, 1.0f)");
						expr = std::format("(({} != kNoTexture) ? g_textures[{}].Sample(g_linearSampler, {}.xy) : float4(1, 1, 1, 1))",
						        SlotExpression(node->slot), SlotExpression(node->slot), uv);
						break;
					}
					case MaterialNodeType::NormalMap:
					{
						// Unpacked from [0,1] and rotated into world space by the same TBN the
						// standard shader builds, so a normal map reads identically in both.
						const std::string uv = Input(nodeId, 0, "float4(input.uv, 0.0f, 1.0f)");
						expr = std::format(
						        "(({} != kNoTexture) ? float4(normalize(mul(g_textures[{}].Sample(g_linearSampler, {}.xy).xyz * 2.0f - 1.0f, graphTBN)), 1.0f) : float4(graphGeometricNormal, 1.0f))",
						        SlotExpression(node->slot), SlotExpression(node->slot), uv);
						break;
					}
					case MaterialNodeType::Panner:
					{
						const std::string uv = Input(nodeId, 0, "float4(input.uv, 0.0f, 1.0f)");
						expr = std::format("float4({}.xy + fc->elapsedTime * float2({}, {}), 0.0f, 1.0f)", uv, node->value[0], node->value[1]);
						break;
					}
					case MaterialNodeType::Noise:
					{
						const std::string uv = Input(nodeId, 0, "float4(input.uv, 0.0f, 1.0f)");
						expr = std::format("GraphValueNoise({}.xy * {}).xxxx", uv, node->value[0]);
						break;
					}
					case MaterialNodeType::Step:
						expr = std::format("step({}, {})", Input(nodeId, 0, "float4(0.5f, 0.5f, 0.5f, 0.5f)"), Input(nodeId, 1, "float4(0, 0, 0, 0)"));
						break;
					case MaterialNodeType::Subtract:
						expr = std::format("({} - {})", Input(nodeId, 0, "float4(0, 0, 0, 0)"), Input(nodeId, 1, "float4(0, 0, 0, 0)"));
						break;
					case MaterialNodeType::Divide:
						expr = std::format("GraphSafeDivide({}, {})", Input(nodeId, 0, "float4(0, 0, 0, 0)"), Input(nodeId, 1, "float4(1, 1, 1, 1)"));
						break;
					case MaterialNodeType::OneMinus:
						expr = std::format("(float4(1, 1, 1, 1) - {})", Input(nodeId, 0, "float4(0, 0, 0, 0)"));
						break;
					case MaterialNodeType::Power:
						// pow() of a negative base is undefined, and a mask arriving slightly
						// below zero is common enough that it would show up as NaN speckle.
						expr = std::format("pow(max({}, 0.0f), {})", Input(nodeId, 0, "float4(0, 0, 0, 0)"), Input(nodeId, 1, "float4(1, 1, 1, 1)"));
						break;
					case MaterialNodeType::Saturate:
						expr = std::format("saturate({})", Input(nodeId, 0, "float4(0, 0, 0, 0)"));
						break;
					case MaterialNodeType::Dot:
						expr = std::format("float4(dot({}.xyz, {}.xyz).xxx, 1.0f)", Input(nodeId, 0, "float4(0, 0, 0, 0)"), Input(nodeId, 1, "float4(0, 0, 0, 0)"));
						break;
					case MaterialNodeType::Smoothstep:
						expr = std::format("smoothstep({}, {}, {})", Input(nodeId, 0, "float4(0, 0, 0, 0)"),
						        Input(nodeId, 1, "float4(1, 1, 1, 1)"), Input(nodeId, 2, "float4(0.5f, 0.5f, 0.5f, 0.5f)"));
						break;
					case MaterialNodeType::Sine:
						expr = std::format("sin({} * {})", Input(nodeId, 0, "float4(fc->elapsedTime, fc->elapsedTime, fc->elapsedTime, fc->elapsedTime)"), node->value[0]);
						break;
					case MaterialNodeType::Remap:
						expr = std::format("GraphRemap({}, {}, {}, {}, {})", Input(nodeId, 0, "float4(0, 0, 0, 0)"),
						        node->value[0], node->value[1], node->value[2], node->value[3]);
						break;
					case MaterialNodeType::Channel:
					{
						constexpr const char* kSwizzles[4] = {"xxxx", "yyyy", "zzzz", "wwww"};
						expr = std::format("({}).{}", Input(nodeId, 0, "float4(0, 0, 0, 0)"), kSwizzles[std::clamp(node->channel, 0, 3)]);
						break;
					}
					case MaterialNodeType::WorldPosition:
						expr = "float4(input.worldPos, 1.0f)";
						break;
					case MaterialNodeType::VertexColor:
						expr = "float4(input.vertexColor, 1.0f)";
						break;
					case MaterialNodeType::ViewDirection:
						expr = "float4(normalize(fc->cameraWorldPos.xyz - input.worldPos), 1.0f)";
						break;
					case MaterialNodeType::Multiply:
						expr = std::format("({} * {})", Input(nodeId, 0, "float4(1, 1, 1, 1)"), Input(nodeId, 1, "float4(1, 1, 1, 1)"));
						break;
					case MaterialNodeType::Add:
						expr = std::format("({} + {})", Input(nodeId, 0, "float4(0, 0, 0, 0)"), Input(nodeId, 1, "float4(0, 0, 0, 0)"));
						break;
					case MaterialNodeType::Lerp:
						expr = std::format("lerp({}, {}, saturate({}.x))", Input(nodeId, 0, "float4(0, 0, 0, 1)"),
						        Input(nodeId, 1, "float4(1, 1, 1, 1)"), Input(nodeId, 2, "float4(0.5f, 0, 0, 1)"));
						break;
					case MaterialNodeType::Output:
					default:
						expr = "float4(0, 0, 0, 1)";
						break;
				}

				body += std::format("    const float4 {} = {};\n", var, expr);
				onStack.erase(nodeId);
				done.emplace(nodeId, var);
				return var;
			}
		};
	} // namespace

	std::string GenerateMaterialShader(const MaterialGraph& graph, std::string& error)
	{
		error.clear();
		const auto outputIt = std::ranges::find(graph.nodes, MaterialNodeType::Output, &MaterialNode::type);
		if (outputIt == graph.nodes.end())
		{
			error = "This graph has no Output node.";
			return {};
		}

		std::string body;
		Emitter emitter{.graph = graph, .body = body};
		const std::string baseColor = emitter.Input(outputIt->id, 0, "float4(0.6f, 0.6f, 0.6f, 1)");
		const std::string metallic = emitter.Input(outputIt->id, 1, "float4(0, 0, 0, 1)");
		const std::string roughness = emitter.Input(outputIt->id, 2, "float4(0.5f, 0, 0, 1)");
		const std::string emissive = emitter.Input(outputIt->id, 3, "float4(0, 0, 0, 1)");
		const std::string normal = emitter.Input(outputIt->id, 4, "float4(graphGeometricNormal, 1)");
		if (!emitter.error.empty())
		{
			error = emitter.error;
			return {};
		}

		// Direct sun plus a sky-coloured ambient, built from the same helpers the standard
		// shader uses. Deliberately no shadows or fog: those need the whole cascade and fog
		// setup, and a generated shader that silently rendered them differently from the rest
		// of the scene would be worse than one that plainly does less.
		return std::format(R"SHADER(// GENERATED by the AetherCore material graph. Edits here are lost on the next compile.
// The bindless heap must be at set 0 - every other set reads garbage on this backend - and
// has to be declared before the headers that sample through it.
[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/GpuMaterial.slangh"
#include "include/Sky.slangh"
#include "include/ShadowSampling.slangh"
#include "include/LocalShadow.slangh"
#include "include/PBRUtilities.slangh"
#include "include/MeshVertex.slangh"
#include "include/DefaultVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

// Value noise: a hashed lattice with a smoothstep between cells. Cheap, deterministic and
// tileable enough for a material, and it needs no texture to be shipped alongside.
float GraphHash21(float2 p)
{{
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453123f);
}}

float GraphValueNoise(float2 p)
{{
    const float2 cell = floor(p);
    const float2 f = p - cell;
    const float2 w = f * f * (3.0f - 2.0f * f);
    const float a = GraphHash21(cell);
    const float b = GraphHash21(cell + float2(1.0f, 0.0f));
    const float c = GraphHash21(cell + float2(0.0f, 1.0f));
    const float d = GraphHash21(cell + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
}}

// Dividing by zero produces an inf that spreads through everything downstream, so the
// divisor is pushed off zero while keeping its sign. sign() returns 0 exactly at zero, which
// is why the 1 is added back in there.
float4 GraphSafeDivide(float4 a, float4 b)
{{
    const float4 s = sign(b);
    return a / ((s + (1.0f - abs(s))) * max(abs(b), 1e-5f));
}}

float4 GraphRemap(float4 x, float inMin, float inMax, float outMin, float outMax)
{{
    const float span = (abs(inMax - inMin) < 1e-6f) ? 1e-6f : (inMax - inMin);
    return outMin + (x - inMin) * ((outMax - outMin) / span);
}}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{{
    FrameConstantsData* fc        = pc.frame.Ptr();
    GpuMaterial*        materials = (GpuMaterial*)(fc->materialBufferAddr);

    GpuMaterial mat;
    mat.albedoSlot = kNoTexture;
    mat.normalSlot = kNoTexture;
    mat.metallicRoughnessSlot = kNoTexture;
    mat.occlusionSlot = kNoTexture;
    mat.emissiveSlot = kNoTexture;
    if (input.materialIndex != kNoTexture)
    {{
        mat = materials[input.materialIndex];
    }}

    // Built before the graph body because a Normal map node rotates through it. Same
    // construction as the standard shader, so a normal map reads the same in both.
    const float3 graphGeometricNormal = normalize(input.worldNormal);
    float3 graphT = input.worldTangent;
    if (dot(graphT, graphT) < 1e-8f)
    {{
        graphT = abs(graphGeometricNormal.y) < 0.99f ? cross(float3(0, 1, 0), graphGeometricNormal)
                                                     : cross(float3(1, 0, 0), graphGeometricNormal);
    }}
    graphT = normalize(graphT - graphGeometricNormal * dot(graphGeometricNormal, graphT));
    const float3 graphB = cross(graphGeometricNormal, graphT) * input.tangentSign;
    const float3x3 graphTBN = float3x3(graphT, graphB, graphGeometricNormal);

{}
    const float3 albedo    = {}.rgb;
    const float  metallic  = saturate({}.x);
    const float  roughness = clamp({}.x, 0.04f, 1.0f);
    const float3 emissive  = {}.rgb;

    const float3 N = normalize({}.xyz);
    const float3 V = normalize(fc->cameraWorldPos.xyz - input.worldPos);
    const float3 L = normalize(-fc->sunDirectionIntensity.xyz);
    const float3 H = normalize(V + L);

    const float NdotL = saturate(dot(N, L));
    const float NdotV = saturate(dot(N, V)) + 1e-5f;
    const float NdotH = saturate(dot(N, H));

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 F  = F_Schlick(saturate(dot(H, V)), F0);
    const float  D  = D_GGX(NdotH, roughness);
    const float  G  = G_SmithSchlick(NdotV, NdotL, roughness);

    const float3 specular = (D * G) * F / max(4.0f * NdotV * NdotL, 1e-4f);
    const float3 diffuse  = (float3(1, 1, 1) - F) * (1.0f - metallic) * albedo / 3.14159265f;
    const float3 direct   = (diffuse + specular) * fc->sunColor.rgb * fc->sunDirectionIntensity.w * NdotL;

    const float3 ambient = SkyGradient(N, normalize(-fc->sunDirectionIntensity.xyz), fc->sunColor.rgb, fc->sunDirectionIntensity.w,
                                       fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams)
                         * albedo * (1.0f - metallic);

    return float4(direct + ambient + emissive, 1.0f);
}}
)SHADER",
		        body, baseColor, metallic, roughness, emissive, normal);
	}
} // namespace aether::editor
