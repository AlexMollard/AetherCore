#include "materialgraph/MaterialGraph.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

#include "utils/TextIni.hpp"

namespace aether::editor
{
	namespace
	{
		struct NodeSpec
		{
			const char* name;
			int inputs;
			const char* inputNames[3];
		};

		const NodeSpec& SpecOf(MaterialNodeType type)
		{
			static const NodeSpec kSpecs[] = {
			        {"Output", 4, {"", "", ""}},
			        {"Colour", 0, {"", "", ""}},
			        {"Float", 0, {"", "", ""}},
			        {"Texture", 0, {"", "", ""}},
			        {"UV", 0, {"", "", ""}},
			        {"Time", 0, {"", "", ""}},
			        {"Fresnel", 0, {"", "", ""}},
			        {"Multiply", 2, {"A", "B", ""}},
			        {"Add", 2, {"A", "B", ""}},
			        {"Lerp", 3, {"A", "B", "T"}},
			};
			return kSpecs[static_cast<std::size_t>(type)];
		}

		const char* kOutputPinNames[4] = {"Base colour", "Metallic", "Roughness", "Emissive"};

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

	int MaterialNodeInputCount(MaterialNodeType type)
	{
		return SpecOf(type).inputs;
	}

	const char* MaterialNodeInputName(MaterialNodeType type, int pin)
	{
		if (type == MaterialNodeType::Output)
		{
			return (pin >= 0 && pin < 4) ? kOutputPinNames[pin] : "";
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
		std::string out = "# AetherCore material graph.\n";
		out += std::format("next_id = {}\n", graph.nextId);
		// Indexed sub-tables rather than [[node]] arrays of tables: text::ParseToml descends
		// into child TABLES but reports an array as one value, so an array of tables reads
		// back as nothing at all.
		int index = 0;
		for (const MaterialNode& node: graph.nodes)
		{
			out += std::format("\n[node.{}]\n", index++);
			out += std::format("id = {}\n", node.id);
			out += std::format("type = {}\n", static_cast<int>(node.type));
			out += std::format("x = {}\n", node.x);
			out += std::format("y = {}\n", node.y);
			out += std::format("value = [ {}, {}, {}, {} ]\n", node.value[0], node.value[1], node.value[2], node.value[3]);
			out += std::format("slot = {}\n", static_cast<int>(node.slot));
		}
		index = 0;
		for (const MaterialLink& link: graph.links)
		{
			out += std::format("\n[link.{}]\n", index++);
			out += std::format("id = {}\n", link.id);
			out += std::format("from_node = {}\n", link.fromNode);
			out += std::format("from_pin = {}\n", link.fromPin);
			out += std::format("to_node = {}\n", link.toNode);
			out += std::format("to_pin = {}\n", link.toPin);
		}
		return out;
	}

	MaterialGraph ParseMaterialGraph(const std::string& text)
	{
		MaterialGraph graph;
		MaterialNode node;
		MaterialLink link;
		std::string current;
		bool inNode = false;
		bool inLink = false;
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

		text::ParseToml(text,
		        [&](const text::IniEntry& entry)
		        {
			        // Each element has its own section - "node.0", "link.3" - so a change of
			        // section is exactly the boundary between two of them.
			        if (entry.section != current)
			        {
				        flush();
				        current = entry.section;
				        if (current.starts_with("node."))
				        {
					        node = MaterialNode{};
					        inNode = true;
				        }
				        else if (current.starts_with("link."))
				        {
					        link = MaterialLink{};
					        inLink = true;
				        }
			        }

			        const auto asInt = [&](int fallback) { return static_cast<int>(text::ParseFloat(entry.value).value_or(static_cast<float>(fallback))); };
			        if (entry.fullKey == "next_id")
			        {
				        graph.nextId = asInt(1);
			        }
			        else if (inNode)
			        {
				        if (entry.key == "id") { node.id = asInt(0); }
				        else if (entry.key == "type") { node.type = static_cast<MaterialNodeType>(asInt(0)); }
				        else if (entry.key == "x") { node.x = text::ParseFloat(entry.value).value_or(0.0f); }
				        else if (entry.key == "y") { node.y = text::ParseFloat(entry.value).value_or(0.0f); }
				        else if (entry.key == "slot") { node.slot = static_cast<MaterialTextureSlot>(asInt(0)); }
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
		if (graph.nodes.empty())
		{
			return MakeDefaultMaterialGraph();
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
						expr = std::format("(({} != kNoTexture) ? g_textures[{}].Sample(g_linearSampler, input.uv) : float4(1, 1, 1, 1))",
						        SlotExpression(node->slot), SlotExpression(node->slot));
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

{}
    const float3 albedo    = {}.rgb;
    const float  metallic  = saturate({}.x);
    const float  roughness = clamp({}.x, 0.04f, 1.0f);
    const float3 emissive  = {}.rgb;

    const float3 N = normalize(input.worldNormal);
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
		        body, baseColor, metallic, roughness, emissive);
	}
} // namespace aether::editor
