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
		constexpr MaterialValueType kAny = MaterialValueType::Any;
		constexpr MaterialValueType kF1 = MaterialValueType::Float;
		constexpr MaterialValueType kF2 = MaterialValueType::Float2;
		constexpr MaterialValueType kF3 = MaterialValueType::Float3;
		constexpr MaterialValueType kF4 = MaterialValueType::Float4;

		struct NodeSpec
		{
			const char* name;
			MaterialNodeCategory category;
			int inputs;
			const char* inputNames[3];
			MaterialValueType inputTypes[3];
			// Any means "as wide as whatever is plugged in", resolved by MaterialNodeOutputType.
			MaterialValueType output;
			const char* description;
		};

		// Indexed by MaterialNodeType, so the order here follows the enum exactly.
		constexpr NodeSpec kSpecs[] = {
		        {"Output", MaterialNodeCategory::Output, 5, {"", "", ""}, {kAny, kAny, kAny}, kF4, "What the surface is made of."},
		        {"Colour", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF4, "A constant colour."},
		        {"Float", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF1, "A constant number."},
		        {"Texture", MaterialNodeCategory::Texture, 1, {"UV", "", ""}, {kF2, kAny, kAny}, kF4, "Samples one of the material's texture slots."},
		        {"UV", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF2, "The mesh's texture coordinates."},
		        {"Time", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF1, "Seconds since the engine started."},
		        {"Fresnel", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF1, "Rim term: bright where the surface faces away from the camera."},
		        {"Multiply", MaterialNodeCategory::Math, 2, {"A", "B", ""}, {kAny, kAny, kAny}, kAny, "A * B."},
		        {"Add", MaterialNodeCategory::Math, 2, {"A", "B", ""}, {kAny, kAny, kAny}, kAny, "A + B."},
		        {"Lerp", MaterialNodeCategory::Math, 3, {"A", "B", "T"}, {kAny, kAny, kF1}, kAny, "Blends from A to B by T."},
		        {"Normal map", MaterialNodeCategory::Texture, 1, {"UV", "", ""}, {kF2, kAny, kAny}, kF3, "Unpacks a tangent-space normal map into world space."},
		        {"Panner", MaterialNodeCategory::Math, 1, {"UV", "", ""}, {kF2, kAny, kAny}, kF2, "Scrolls a UV over time."},
		        {"Noise", MaterialNodeCategory::Math, 1, {"UV", "", ""}, {kF2, kAny, kAny}, kF1, "Value noise over a UV."},
		        {"Step", MaterialNodeCategory::Math, 2, {"Edge", "X", ""}, {kAny, kAny, kAny}, kAny, "0 below the edge, 1 above it. A hard cut."},
		        {"Subtract", MaterialNodeCategory::Math, 2, {"A", "B", ""}, {kAny, kAny, kAny}, kAny, "A - B."},
		        {"Divide", MaterialNodeCategory::Math, 2, {"A", "B", ""}, {kAny, kAny, kAny}, kAny, "A / B, guarded against dividing by zero."},
		        {"One minus", MaterialNodeCategory::Math, 1, {"X", "", ""}, {kAny, kAny, kAny}, kAny, "1 - X. Inverts a mask."},
		        {"Power", MaterialNodeCategory::Math, 2, {"X", "Exp", ""}, {kAny, kF1, kAny}, kAny, "X raised to Exp. Sharpens a gradient."},
		        {"Saturate", MaterialNodeCategory::Math, 1, {"X", "", ""}, {kAny, kAny, kAny}, kAny, "Clamps to 0..1."},
		        {"Dot", MaterialNodeCategory::Math, 2, {"A", "B", ""}, {kF3, kF3, kAny}, kF1, "Dot product of two directions."},
		        {"Smoothstep", MaterialNodeCategory::Math, 3, {"Edge 0", "Edge 1", "X"}, {kAny, kAny, kAny}, kAny, "A soft Step: eased from 0 to 1 between the edges."},
		        {"Sine", MaterialNodeCategory::Math, 1, {"X", "", ""}, {kAny, kAny, kAny}, kAny, "sin(X * frequency), for anything that pulses."},
		        {"Remap", MaterialNodeCategory::Math, 1, {"X", "", ""}, {kAny, kAny, kAny}, kAny, "Rescales X from one range to another."},
		        {"Channel", MaterialNodeCategory::Math, 1, {"X", "", ""}, {kAny, kAny, kAny}, kF1, "Picks one channel out of a colour or vector."},
		        {"World position", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF3, "The shaded point, in world space."},
		        {"Vertex colour", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF3, "The mesh's per-vertex colour."},
		        {"View direction", MaterialNodeCategory::Input, 0, {"", "", ""}, {kAny, kAny, kAny}, kF3, "Unit vector from the surface towards the camera."},
		};
		static_assert(std::size(kSpecs) == static_cast<std::size_t>(MaterialNodeType::ViewDirection) + 1,
		        "Every MaterialNodeType needs a spec; the table is indexed by the enum.");

		const NodeSpec& SpecOf(MaterialNodeType type)
		{
			return kSpecs[static_cast<std::size_t>(type)];
		}

		const char* kOutputPinNames[5] = {"Base colour", "Metallic", "Roughness", "Emissive", "Normal"};
		// What the lighting model needs from each Output pin: colours are float3, the scalar
		// factors are floats, and the normal is a direction.
		constexpr MaterialValueType kOutputPinTypes[5] = {kF3, kF1, kF1, kF3, kF3};

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

		// A literal of the given width, so a fallback for an unconnected pin is typed like
		// everything else rather than always being a float4.
		std::string Literal(MaterialValueType type, float v)
		{
			switch (type)
			{
				case MaterialValueType::Float2:
					return std::format("float2({}, {})", v, v);
				case MaterialValueType::Float3:
					return std::format("float3({}, {}, {})", v, v, v);
				case MaterialValueType::Float4:
					return std::format("float4({}, {}, {}, {})", v, v, v, v);
				case MaterialValueType::Float:
				case MaterialValueType::Any:
				default:
					return std::format("{}", v);
			}
		}

		// Rewrites an expression of one width into another. Only the conversions
		// MaterialCanConnect allows ever reach this.
		std::string Adapt(const std::string& expr, MaterialValueType from, MaterialValueType to)
		{
			if (from == to || to == MaterialValueType::Any)
			{
				return expr;
			}
			const int fromN = MaterialValueComponents(from);
			const int toN = MaterialValueComponents(to);
			if (fromN == 1)
			{
				// A single float fills every component, which is what everyone means by
				// plugging a mask into a colour.
				return std::format("{}({})", MaterialValueTypeName(to), expr);
			}
			if (fromN > toN)
			{
				constexpr const char* kSwizzle[5] = {"", "x", "xy", "xyz", "xyzw"};
				return std::format("({}).{}", expr, kSwizzle[toN]);
			}
			// Widening a vector is refused at connect time; padding here only keeps the
			// generator total for a graph written by hand.
			std::string out = std::format("{}({}", MaterialValueTypeName(to), expr);
			for (int i = fromN; i < toN; ++i)
			{
				out += ", 0";
			}
			out += ")";
			return out;
		}
	} // namespace

	int MaterialValueComponents(MaterialValueType type)
	{
		switch (type)
		{
			case MaterialValueType::Float2:
				return 2;
			case MaterialValueType::Float3:
				return 3;
			case MaterialValueType::Float4:
				return 4;
			case MaterialValueType::Float:
			case MaterialValueType::Any:
			default:
				return 1;
		}
	}

	const char* MaterialValueTypeName(MaterialValueType type)
	{
		switch (type)
		{
			case MaterialValueType::Float2:
				return "float2";
			case MaterialValueType::Float3:
				return "float3";
			case MaterialValueType::Float4:
				return "float4";
			case MaterialValueType::Float:
				return "float";
			case MaterialValueType::Any:
			default:
				return "any";
		}
	}

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

	MaterialValueType MaterialNodeInputType(MaterialNodeType type, int pin)
	{
		if (type == MaterialNodeType::Output)
		{
			return (pin >= 0 && pin < 5) ? kOutputPinTypes[pin] : MaterialValueType::Any;
		}
		const NodeSpec& spec = SpecOf(type);
		return (pin >= 0 && pin < spec.inputs) ? spec.inputTypes[pin] : MaterialValueType::Any;
	}

	namespace
	{
		// Depth-limited rather than visited-set based: an adaptive node's width depends on its
		// inputs, so a cycle would recurse forever. GenerateMaterialShader reports the cycle
		// properly; this only has to terminate.
		MaterialValueType ResolveOutputType(const MaterialGraph& graph, int nodeId, int depth)
		{
			const MaterialNode* node = graph.Find(nodeId);
			if (node == nullptr || depth > 64)
			{
				return MaterialValueType::Float;
			}
			const NodeSpec& spec = SpecOf(node->type);
			if (spec.output != MaterialValueType::Any)
			{
				return spec.output;
			}
			// As wide as the widest thing plugged in. An adaptive node with nothing connected
			// is a float, which is what its unconnected-input fallbacks are.
			MaterialValueType widest = MaterialValueType::Float;
			for (int pin = 0; pin < spec.inputs; ++pin)
			{
				// A pin that declares a width does not widen the node: Lerp's T is always a
				// float, and Power's exponent likewise.
				if (spec.inputTypes[pin] != MaterialValueType::Any)
				{
					continue;
				}
				if (const MaterialLink* link = graph.LinkInto(nodeId, pin); link != nullptr)
				{
					const MaterialValueType incoming = ResolveOutputType(graph, link->fromNode, depth + 1);
					if (MaterialValueComponents(incoming) > MaterialValueComponents(widest))
					{
						widest = incoming;
					}
				}
			}
			return widest;
		}
	} // namespace

	MaterialValueType MaterialNodeOutputType(const MaterialGraph& graph, int nodeId)
	{
		return ResolveOutputType(graph, nodeId, 0);
	}

	MaterialConnection MaterialCanConnect(MaterialValueType from, MaterialValueType to)
	{
		if (to == MaterialValueType::Any || from == to)
		{
			return MaterialConnection::Exact;
		}
		if (from == MaterialValueType::Float)
		{
			return MaterialConnection::Broadcast;
		}
		if (MaterialValueComponents(from) > MaterialValueComponents(to))
		{
			return MaterialConnection::Truncate;
		}
		return MaterialConnection::Refused;
	}

	std::string MaterialConnectionRefusal(MaterialValueType from, MaterialValueType to)
	{
		if (MaterialCanConnect(from, to) != MaterialConnection::Refused)
		{
			return {};
		}
		// The only refused case is a narrow vector driving a wider input. There is no one
		// right way to invent the missing components, so the graph does not guess.
		return std::format("A {} cannot fill a {} input - there is no obvious value for the missing components. A single float would broadcast.",
		        MaterialValueTypeName(from), MaterialValueTypeName(to));
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
		// Emits one temporary per node, memoised, so a node feeding three inputs is evaluated
		// once. Every temporary carries the node's real width, and every use of it is adapted
		// to the width the consuming pin declares.
		struct Emitter
		{
			const MaterialGraph& graph;
			std::string& body;
			std::unordered_map<int, std::string> done;
			std::unordered_set<int> onStack;
			std::string error;

			// The value feeding `pin`, already converted to `want`.
			std::string Input(int nodeId, int pin, MaterialValueType want, float fallback)
			{
				const MaterialLink* link = graph.LinkInto(nodeId, pin);
				if (link == nullptr)
				{
					return Literal(want, fallback);
				}
				const std::string var = Visit(link->fromNode);
				return Adapt(var, MaterialNodeOutputType(graph, link->fromNode), want);
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
					return "0";
				}

				const MaterialNode* node = graph.Find(nodeId);
				if (node == nullptr)
				{
					onStack.erase(nodeId);
					return "0";
				}

				const MaterialValueType outType = MaterialNodeOutputType(graph, nodeId);
				const std::string var = std::format("n{}", nodeId);
				std::string expr;
				switch (node->type)
				{
					case MaterialNodeType::ConstantColor:
						expr = std::format("float4({}, {}, {}, {})", node->value[0], node->value[1], node->value[2], node->value[3]);
						break;
					case MaterialNodeType::ConstantFloat:
						expr = std::format("{}", node->value[0]);
						break;
					case MaterialNodeType::Uv:
						expr = "input.uv";
						break;
					case MaterialNodeType::Time:
						expr = "fc->elapsedTime";
						break;
					case MaterialNodeType::Fresnel:
						// Schlick with a fixed power: the rim term everyone reaches for first.
						expr = "pow(1.0f - saturate(dot(normalize(input.worldNormal), normalize(fc->cameraWorldPos.xyz - input.worldPos))), 5.0f)";
						break;
					case MaterialNodeType::TextureSample:
					{
						// Sampling at the node's own UV rather than the interpolated one is
						// the whole point of having a Panner or any other UV maths upstream.
						const std::string uv = Input(nodeId, 0, kF2, 0.0f);
						expr = std::format("(({} != kNoTexture) ? g_textures[{}].Sample(g_linearSampler, {}) : float4(1, 1, 1, 1))",
						        SlotExpression(node->slot), SlotExpression(node->slot), uv);
						break;
					}
					case MaterialNodeType::NormalMap:
					{
						// Unpacked from [0,1] and rotated into world space by the same TBN the
						// standard shader builds, so a normal map reads identically in both.
						const std::string uv = Input(nodeId, 0, kF2, 0.0f);
						expr = std::format(
						        "(({} != kNoTexture) ? normalize(mul(g_textures[{}].Sample(g_linearSampler, {}).xyz * 2.0f - 1.0f, graphTBN)) : graphGeometricNormal)",
						        SlotExpression(node->slot), SlotExpression(node->slot), uv);
						break;
					}
					case MaterialNodeType::Panner:
						expr = std::format("({} + fc->elapsedTime * float2({}, {}))", Input(nodeId, 0, kF2, 0.0f), node->value[0], node->value[1]);
						break;
					case MaterialNodeType::Noise:
						expr = std::format("GraphValueNoise({} * {})", Input(nodeId, 0, kF2, 0.0f), node->value[0]);
						break;
					case MaterialNodeType::Step:
						expr = std::format("step({}, {})", Input(nodeId, 0, outType, 0.5f), Input(nodeId, 1, outType, 0.0f));
						break;
					case MaterialNodeType::Multiply:
						expr = std::format("({} * {})", Input(nodeId, 0, outType, 1.0f), Input(nodeId, 1, outType, 1.0f));
						break;
					case MaterialNodeType::Add:
						expr = std::format("({} + {})", Input(nodeId, 0, outType, 0.0f), Input(nodeId, 1, outType, 0.0f));
						break;
					case MaterialNodeType::Subtract:
						expr = std::format("({} - {})", Input(nodeId, 0, outType, 0.0f), Input(nodeId, 1, outType, 0.0f));
						break;
					case MaterialNodeType::Divide:
						expr = std::format("GraphSafeDivide({}, {})", Input(nodeId, 0, outType, 0.0f), Input(nodeId, 1, outType, 1.0f));
						break;
					case MaterialNodeType::Lerp:
						expr = std::format("lerp({}, {}, saturate({}))", Input(nodeId, 0, outType, 0.0f), Input(nodeId, 1, outType, 1.0f),
						        Input(nodeId, 2, kF1, 0.5f));
						break;
					case MaterialNodeType::OneMinus:
						expr = std::format("(1.0f - {})", Input(nodeId, 0, outType, 0.0f));
						break;
					case MaterialNodeType::Power:
						// pow() of a negative base is undefined, and a mask arriving slightly
						// below zero is common enough that it would show up as NaN speckle.
						expr = std::format("pow(max({}, 0.0f), {})", Input(nodeId, 0, outType, 0.0f), Input(nodeId, 1, kF1, 1.0f));
						break;
					case MaterialNodeType::Saturate:
						expr = std::format("saturate({})", Input(nodeId, 0, outType, 0.0f));
						break;
					case MaterialNodeType::Dot:
						expr = std::format("dot({}, {})", Input(nodeId, 0, kF3, 0.0f), Input(nodeId, 1, kF3, 0.0f));
						break;
					case MaterialNodeType::Smoothstep:
						expr = std::format("smoothstep({}, {}, {})", Input(nodeId, 0, outType, 0.0f), Input(nodeId, 1, outType, 1.0f),
						        Input(nodeId, 2, outType, 0.5f));
						break;
					case MaterialNodeType::Sine:
						expr = std::format("sin({} * {})", Input(nodeId, 0, outType, 0.0f), node->value[0]);
						break;
					case MaterialNodeType::Remap:
						expr = std::format("GraphRemap({}, {}, {}, {}, {})", Input(nodeId, 0, outType, 0.0f), node->value[0], node->value[1],
						        node->value[2], node->value[3]);
						break;
					case MaterialNodeType::Channel:
					{
						// Clamped to what the incoming value actually has: asking for .w of a
						// float3 is a compile error, not a black pixel.
						const MaterialLink* link = graph.LinkInto(nodeId, 0);
						const MaterialValueType inType = link != nullptr ? MaterialNodeOutputType(graph, link->fromNode) : kF1;
						const int available = MaterialValueComponents(inType);
						const int channel = std::clamp(node->channel, 0, available - 1);
						constexpr const char* kComponent[4] = {"x", "y", "z", "w"};
						expr = std::format("({}).{}", Input(nodeId, 0, inType, 0.0f), kComponent[channel]);
						if (inType == kF1)
						{
							expr = Input(nodeId, 0, kF1, 0.0f);
						}
						break;
					}
					case MaterialNodeType::WorldPosition:
						expr = "input.worldPos";
						break;
					case MaterialNodeType::VertexColor:
						expr = "input.vertexColor";
						break;
					case MaterialNodeType::ViewDirection:
						expr = "normalize(fc->cameraWorldPos.xyz - input.worldPos)";
						break;
					case MaterialNodeType::Output:
					default:
						expr = Literal(outType, 0.0f);
						break;
				}

				body += std::format("    const {} {} = {};\n", MaterialValueTypeName(outType), var, expr);
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
		const std::string baseColor = emitter.Input(outputIt->id, 0, MaterialValueType::Float3, 0.6f);
		const std::string metallic = emitter.Input(outputIt->id, 1, MaterialValueType::Float, 0.0f);
		const std::string roughness = emitter.Input(outputIt->id, 2, MaterialValueType::Float, 0.5f);
		const std::string emissive = emitter.Input(outputIt->id, 3, MaterialValueType::Float3, 0.0f);
		const MaterialLink* normalLink = graph.LinkInto(outputIt->id, 4);
		const std::string normal = normalLink != nullptr ? emitter.Input(outputIt->id, 4, MaterialValueType::Float3, 0.0f) : std::string("graphGeometricNormal");
		if (!emitter.error.empty())
		{
			error = emitter.error;
			return {};
		}

		// Sun, cascade shadows and a sky-based ambient - diffuse AND specular - all through
		// the same helpers gltf_mesh uses, so a graph-driven material sits in a scene looking
		// like the materials around it. Still missing next to the standard shader: local
		// lights, fog, and the occlusion texture, all of which need inputs the graph's Output
		// node does not carry yet.
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
#include "include/DirectionalShadowCSM.slangh"
#include "include/LocalShadow.slangh"
#include "include/PBRUtilities.slangh"
#include "include/EnvBrdf.slangh"
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
// is why the 1 is added back in there. One overload per width, because the graph now emits
// values of the width the nodes actually carry.
float  GraphSafeDivide(float  a, float  b) {{ const float  s = sign(b); return a / ((s + (1.0f - abs(s))) * max(abs(b), 1e-5f)); }}
float2 GraphSafeDivide(float2 a, float2 b) {{ const float2 s = sign(b); return a / ((s + (1.0f - abs(s))) * max(abs(b), 1e-5f)); }}
float3 GraphSafeDivide(float3 a, float3 b) {{ const float3 s = sign(b); return a / ((s + (1.0f - abs(s))) * max(abs(b), 1e-5f)); }}
float4 GraphSafeDivide(float4 a, float4 b) {{ const float4 s = sign(b); return a / ((s + (1.0f - abs(s))) * max(abs(b), 1e-5f)); }}

float  GraphRemap(float  x, float inMin, float inMax, float outMin, float outMax) {{ const float span = (abs(inMax - inMin) < 1e-6f) ? 1e-6f : (inMax - inMin); return outMin + (x - inMin) * ((outMax - outMin) / span); }}
float2 GraphRemap(float2 x, float inMin, float inMax, float outMin, float outMax) {{ const float span = (abs(inMax - inMin) < 1e-6f) ? 1e-6f : (inMax - inMin); return outMin + (x - inMin) * ((outMax - outMin) / span); }}
float3 GraphRemap(float3 x, float inMin, float inMax, float outMin, float outMax) {{ const float span = (abs(inMax - inMin) < 1e-6f) ? 1e-6f : (inMax - inMin); return outMin + (x - inMin) * ((outMax - outMin) / span); }}
float4 GraphRemap(float4 x, float inMin, float inMax, float outMin, float outMax) {{ const float span = (abs(inMax - inMin) < 1e-6f) ? 1e-6f : (inMax - inMin); return outMin + (x - inMin) * ((outMax - outMin) / span); }}

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
    const float3 albedo    = {};
    const float  metallic  = saturate({});
    const float  roughness = clamp({}, 0.04f, 1.0f);
    const float3 emissive  = {};

    const float3 N = normalize({});
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
    const float3 kD       = (float3(1, 1, 1) - F) * (1.0f - metallic);
    const float3 diffuse  = kD * albedo / 3.14159265f;

    // The same cascade the standard shader samples, through the same helper. An earlier
    // version left shadows out on the grounds that a generated shader should not invent its
    // own - but calling the engine's own function is not inventing anything, and a material
    // that stayed lit inside every shadow was the more obviously wrong of the two.
    const float sunVisibility = ComputeDirectionalShadow(fc, input.worldPos, graphGeometricNormal, L, input.svPosition.xy);
    const float3 direct = (diffuse + specular) * fc->sunColor.rgb * fc->sunDirectionIntensity.w * NdotL * sunVisibility;

    // Ambient, matching gltf_mesh: a sky-coloured diffuse probe along the normal AND a
    // specular probe along the reflection. Without the specular half a metal has nothing to
    // reflect and renders almost black, which is what every graph-driven metal did.
    const float3 sunDir = normalize(-fc->sunDirectionIntensity.xyz);
    const float  sunIntensity = fc->sunDirectionIntensity.w;
    const float3 skyDiffuse = SkyGradient(N, sunDir, fc->sunColor.rgb, sunIntensity,
                                          fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams);
    // One sample standing in for a prefiltered mip chain: the probe direction drifts from
    // the mirror reflection towards the normal as the surface roughens.
    const float3 R = reflect(-V, N);
    const float3 skySpecularDir = normalize(lerp(R, N, roughness * roughness));
    const float3 skySpecular = SkyGradient(skySpecularDir, sunDir, fc->sunColor.rgb, sunIntensity,
                                           fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams);

    const float2 envBrdf = EnvBrdfApprox(roughness, NdotV);
    // ambientColor is the scene's authored floor - the light that is not the sky at all.
    const float3 ambientDiffuse  = (fc->ambientColor.rgb + skyDiffuse) * albedo * kD;
    // Multi-scatter compensated, so a rough surface stops losing the light that bounces
    // between its own microfacets.
    const float3 ambientSpecular = skySpecular * EnvSpecularWeight(F0, envBrdf);

    return float4(direct + ambientDiffuse + ambientSpecular + emissive, 1.0f);
}}
)SHADER",
		        body, baseColor, metallic, roughness, emissive, normal);
	}
} // namespace aether::editor
