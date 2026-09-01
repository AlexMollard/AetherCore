#include "debug/MaterialGraphPanel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imnodes.h>

#include "assets/AssetManager.hpp"
#include "debug/EditorChrome.hpp"
#include "editor/EditorProjectContext.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "editor/ShaderCompiler.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "layers/AppLayer.hpp"
#include "material/PipelineCache.hpp"
#include "material/PipelineCache.hpp"
#include "material/TextureRegistry.hpp"
#include <utility>
#include "mesh/PrimitiveMeshes.hpp"
#include "rendering/ModelPreviewService.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		// ImNodes addresses pins by a flat integer id, so node and pin are packed into one.
		// A stride of 16 leaves room for the five Output inputs plus an output pin, and keeps
		// pin ids from aliasing onto node ids.
		constexpr int kPinStride = 16;
		constexpr int kOutputPinSlot = 8;

		int InputPinId(int nodeId, int pin) { return nodeId * kPinStride + pin; }
		int OutputPinId(int nodeId) { return nodeId * kPinStride + kOutputPinSlot; }
		int NodeOfPin(int pinId) { return pinId / kPinStride; }
		bool IsOutputPin(int pinId) { return (pinId % kPinStride) == kOutputPinSlot; }
		int PinIndex(int pinId) { return pinId % kPinStride; }

		// ImNodes ships a blue-and-grey palette of its own, which sits in the middle of the
		// editor looking like a different application. Every colour is rebuilt from the
		// editor's theme tokens instead - and rebuilt every frame, cheaply, so switching the
		// editor theme takes the graph with it rather than leaving it on the old one.
		void ApplyGraphTheme()
		{
			ImNodesStyle& style = ImNodes::GetStyle();
			const auto set = [&](ImNodesCol slot, const ImVec4& colour) { style.Colors[slot] = ImGui::ColorConvertFloat4ToU32(colour); };

			set(ImNodesCol_GridBackground, chrome::kBg);
			set(ImNodesCol_GridLine, chrome::WithAlpha(chrome::kStroke, 0.35f));
			set(ImNodesCol_GridLinePrimary, chrome::WithAlpha(chrome::kStroke, 0.65f));

			set(ImNodesCol_NodeBackground, chrome::kPanel);
			set(ImNodesCol_NodeBackgroundHovered, chrome::kPanelHi);
			set(ImNodesCol_NodeBackgroundSelected, chrome::kPanelHi);
			set(ImNodesCol_NodeOutline, chrome::kStroke);

			// The title bar is where the accent belongs: it marks which node is selected
			// without repainting the whole node.
			set(ImNodesCol_TitleBar, chrome::kPanelHi);
			set(ImNodesCol_TitleBarHovered, chrome::kAccentDim);
			set(ImNodesCol_TitleBarSelected, chrome::kAccent);

			// Links and pins are pushed per item, coloured by the width they carry; these are
			// only the fallbacks and the interaction states.
			set(ImNodesCol_Link, chrome::kMuted);
			set(ImNodesCol_LinkHovered, chrome::kAccentHi);
			set(ImNodesCol_LinkSelected, chrome::kAccent);
			set(ImNodesCol_Pin, chrome::kMuted);
			set(ImNodesCol_PinHovered, chrome::kAccentHi);

			set(ImNodesCol_BoxSelector, chrome::WithAlpha(chrome::kAccent, 0.20f));
			set(ImNodesCol_BoxSelectorOutline, chrome::kAccent);

			set(ImNodesCol_MiniMapBackground, chrome::WithAlpha(chrome::kBg, 0.75f));
			set(ImNodesCol_MiniMapBackgroundHovered, chrome::WithAlpha(chrome::kBg, 0.90f));
			set(ImNodesCol_MiniMapOutline, chrome::kStroke);
			set(ImNodesCol_MiniMapOutlineHovered, chrome::kAccent);
			set(ImNodesCol_MiniMapNodeBackground, chrome::kPanelHi);
			set(ImNodesCol_MiniMapNodeBackgroundHovered, chrome::kAccentDim);
			set(ImNodesCol_MiniMapNodeBackgroundSelected, chrome::kAccent);
			set(ImNodesCol_MiniMapNodeOutline, chrome::kStroke);
			set(ImNodesCol_MiniMapLink, chrome::kMuted);
			set(ImNodesCol_MiniMapLinkSelected, chrome::kAccent);
			set(ImNodesCol_MiniMapCanvas, chrome::WithAlpha(chrome::kAccent, 0.08f));
			set(ImNodesCol_MiniMapCanvasOutline, chrome::WithAlpha(chrome::kAccent, 0.45f));

			// Geometry taken from the editor's own style, so nodes are rounded and padded
			// like every other surface rather than to ImNodes' defaults.
			const ImGuiStyle& imgui = ImGui::GetStyle();
			style.NodeCornerRounding = imgui.FrameRounding + 2.0f;
			style.NodeBorderThickness = 1.0f;
			style.NodePadding = ImVec2(10.0f, 8.0f);
			style.LinkThickness = 2.6f;
			style.PinCircleRadius = 4.5f;
		}

		// One colour per width, so what a pin carries is readable without hovering it. Sized
		// to be distinguishable at a glance rather than to be pretty: grey scalars, and warmer
		// colours as the value gets wider.
		ImU32 TypeColour(MaterialValueType type)
		{
			switch (type)
			{
				case MaterialValueType::Float2:
					return IM_COL32(120, 205, 130, 255);
				case MaterialValueType::Float3:
					return IM_COL32(235, 190, 95, 255);
				case MaterialValueType::Float4:
					return IM_COL32(225, 120, 180, 255);
				case MaterialValueType::Float:
				case MaterialValueType::Any:
				default:
					return IM_COL32(175, 180, 190, 255);
			}
		}

		// The width shown beside a pin name. An adaptive pin has no width of its own, so it
		// shows what it will actually become rather than "any", which tells nobody anything.
		const char* TypeSuffix(MaterialValueType type)
		{
			switch (type)
			{
				case MaterialValueType::Float2:
					return "2";
				case MaterialValueType::Float3:
					return "3";
				case MaterialValueType::Float4:
					return "4";
				case MaterialValueType::Float:
				default:
					return "1";
			}
		}

		// Compiling runs slangc as a subprocess, so it waits for a pause rather than firing on
		// every mouse-move of a slider.
		constexpr float kAutoCompileIdleSeconds = 0.35f;

		std::string MaterialStem(const std::string& materialPath)
		{
			// "Rock.material.toml" -> "Rock"; a plain "Rock.toml" -> "Rock".
			return std::filesystem::path(materialPath).stem().stem().generic_string();
		}

		// The sidecar the graph used to live in, before a material became one file.
		std::string LegacyGraphPath(const std::string& materialPath)
		{
			const std::filesystem::path p(materialPath);
			return (p.parent_path() / (MaterialStem(materialPath) + ".materialgraph.toml")).generic_string();
		}

		// The real file behind a virtual path, so the sidecar can actually be deleted:
		// std::filesystem knows nothing about "project://", and removing that path silently
		// did nothing, which left the migration only half done.
		std::filesystem::path PhysicalPathFor(app::LayerContext& context, const std::string& virtualPath)
		{
			constexpr std::string_view kProject = "project://";
			if (virtualPath.starts_with(kProject))
			{
				const auto* project = context.TryGet<app::EditorProjectContext>();
				if (project == nullptr || !project->IsLoaded())
				{
					return {};
				}
				return std::filesystem::path(project->root) / virtualPath.substr(kProject.size());
			}
			return std::filesystem::path(virtualPath);
		}

		// Seeds a graph from a material's current values AND its textures, so moving an
		// existing material onto the node editor starts from what it already looks like. An
		// earlier version seeded only the factors, which quietly threw away the albedo map of
		// every textured material it was used on.
		MaterialGraph GraphFromMaterial(const MaterialPresetSpec& spec)
		{
			MaterialGraph graph;
			int nextId = 1;
			const auto add = [&](MaterialNodeType type, float x, float y) -> MaterialNode&
			{
				MaterialNode node = MakeMaterialNode(nextId++, type);
				node.x = x;
				node.y = y;
				graph.nodes.push_back(node);
				return graph.nodes.back();
			};
			const auto connect = [&](int fromNode, int toNode, int toPin)
			{
				graph.links.push_back(MaterialLink{.id = nextId++, .fromNode = fromNode, .fromPin = 0, .toNode = toNode, .toPin = toPin});
			};

			const int outputId = add(MaterialNodeType::Output, 700.0f, 200.0f).id;

			// Base colour: the factor, multiplied by the albedo map when there is one, which
			// is exactly what the standard shader does with the same two inputs.
			const int colourId = [&]
			{
				MaterialNode& colour = add(MaterialNodeType::ConstantColor, 60.0f, 60.0f);
				for (int i = 0; i < 4; ++i)
				{
					colour.value[i] = spec.material.baseColorFactor[i];
				}
				return colour.id;
			}();
			if (!spec.albedoPath.empty())
			{
				const int texId = add(MaterialNodeType::TextureSample, 60.0f, 170.0f).id;
				const int mulId = add(MaterialNodeType::Multiply, 380.0f, 100.0f).id;
				connect(colourId, mulId, 0);
				connect(texId, mulId, 1);
				connect(mulId, outputId, 0);
			}
			else
			{
				connect(colourId, outputId, 0);
			}

			// Metallic and roughness: from the packed map's blue and green channels when it
			// exists, scaled by the factors, and from the factors alone otherwise.
			const int metallicId = [&]
			{
				MaterialNode& metallic = add(MaterialNodeType::ConstantFloat, 60.0f, 300.0f);
				metallic.value[0] = metallic.value[1] = metallic.value[2] = spec.material.metallicFactor;
				return metallic.id;
			}();
			const int roughnessId = [&]
			{
				MaterialNode& roughness = add(MaterialNodeType::ConstantFloat, 60.0f, 400.0f);
				roughness.value[0] = roughness.value[1] = roughness.value[2] = spec.material.roughnessFactor;
				return roughness.id;
			}();
			if (!spec.metallicRoughnessPath.empty())
			{
				MaterialNode& mrTex = add(MaterialNodeType::TextureSample, 60.0f, 500.0f);
				mrTex.slot = MaterialTextureSlot::MetallicRoughness;
				const int mrId = mrTex.id;

				MaterialNode& metalChannel = add(MaterialNodeType::Channel, 380.0f, 300.0f);
				metalChannel.channel = 2; // glTF packs metallic in blue,
				const int metalChannelId = metalChannel.id;
				MaterialNode& roughChannel = add(MaterialNodeType::Channel, 380.0f, 420.0f);
				roughChannel.channel = 1; // and roughness in green.
				const int roughChannelId = roughChannel.id;
				connect(mrId, metalChannelId, 0);
				connect(mrId, roughChannelId, 0);

				const int metalMul = add(MaterialNodeType::Multiply, 540.0f, 300.0f).id;
				const int roughMul = add(MaterialNodeType::Multiply, 540.0f, 420.0f).id;
				connect(metalChannelId, metalMul, 0);
				connect(metallicId, metalMul, 1);
				connect(roughChannelId, roughMul, 0);
				connect(roughnessId, roughMul, 1);
				connect(metalMul, outputId, 1);
				connect(roughMul, outputId, 2);
			}
			else
			{
				connect(metallicId, outputId, 1);
				connect(roughnessId, outputId, 2);
			}

			if (!spec.normalPath.empty())
			{
				MaterialNode& normal = add(MaterialNodeType::NormalMap, 380.0f, 560.0f);
				normal.slot = MaterialTextureSlot::Normal;
				connect(normal.id, outputId, 4);
			}

			const glm::vec3& e = spec.material.emissiveFactor;
			if (!spec.emissivePath.empty() || e.x > 0.0f || e.y > 0.0f || e.z > 0.0f)
			{
				MaterialNode& emissive = add(MaterialNodeType::ConstantColor, 60.0f, 640.0f);
				emissive.value[0] = e.x;
				emissive.value[1] = e.y;
				emissive.value[2] = e.z;
				emissive.value[3] = 1.0f;
				int emissiveId = emissive.id;
				if (!spec.emissivePath.empty())
				{
					MaterialNode& tex = add(MaterialNodeType::TextureSample, 60.0f, 730.0f);
					tex.slot = MaterialTextureSlot::Emissive;
					const int mulId = add(MaterialNodeType::Multiply, 380.0f, 660.0f).id;
					connect(emissiveId, mulId, 0);
					connect(tex.id, mulId, 1);
					emissiveId = mulId;
				}
				connect(emissiveId, outputId, 3);
			}

			graph.nextId = nextId;
			return graph;
		}
	} // namespace

	std::string MaterialGraphPanel::GraphSignature() const
	{
		return m_graph ? SerializeMaterialGraph(*m_graph) : std::string{};
	}

	void MaterialGraphPanel::SyncLinks()
	{
		int startPin = 0;
		int endPin = 0;
		if (ImNodes::IsLinkCreated(&startPin, &endPin))
		{
			// A drag can begin at either end, so normalise to output then input.
			if (!IsOutputPin(startPin))
			{
				std::swap(startPin, endPin);
			}
			if (IsOutputPin(startPin) && !IsOutputPin(endPin))
			{
				const int fromNode = NodeOfPin(startPin);
				const int toNode = NodeOfPin(endPin);
				const int toPin = PinIndex(endPin);
				const MaterialNode* target = m_graph->Find(toNode);
				if (target == nullptr)
				{
					return;
				}

				// Widths are checked here rather than at compile time, because a connection
				// that cannot work should never be made in the first place.
				const MaterialValueType from = MaterialNodeOutputType(*m_graph, fromNode);
				const MaterialValueType to = MaterialNodeInputType(target->type, toPin);
				if (const std::string refusal = MaterialConnectionRefusal(from, to); !refusal.empty())
				{
					m_status = refusal;
					m_statusIsError = true;
					return;
				}

				// An input takes one link; connecting again replaces rather than stacks.
				std::erase_if(m_graph->links, [&](const MaterialLink& l) { return l.toNode == toNode && l.toPin == toPin; });
				m_graph->links.push_back(MaterialLink{
				        .id = m_graph->nextId++, .fromNode = fromNode, .fromPin = 0, .toNode = toNode, .toPin = toPin});
				m_status.clear();
				m_statusIsError = false;
			}
		}

		int destroyed = 0;
		if (ImNodes::IsLinkDestroyed(&destroyed))
		{
			std::erase_if(m_graph->links, [&](const MaterialLink& l) { return l.id == destroyed; });
		}
	}

	void MaterialGraphPanel::DeleteSelection()
	{
		if (!ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::IsKeyPressed(ImGuiKey_X))
		{
			return;
		}

		if (const int count = ImNodes::NumSelectedLinks(); count > 0)
		{
			std::vector<int> selected(static_cast<std::size_t>(count));
			ImNodes::GetSelectedLinks(selected.data());
			std::erase_if(m_graph->links, [&](const MaterialLink& l) { return std::ranges::find(selected, l.id) != selected.end(); });
			ImNodes::ClearLinkSelection();
		}

		if (const int count = ImNodes::NumSelectedNodes(); count > 0)
		{
			std::vector<int> selected(static_cast<std::size_t>(count));
			ImNodes::GetSelectedNodes(selected.data());
			// The Output node is the graph's reason to exist; deleting it would leave a graph
			// that cannot generate anything, so it is kept whatever the selection says.
			std::erase_if(selected, [&](int id)
			        {
				        const MaterialNode* node = m_graph->Find(id);
				        return node == nullptr || node->type == MaterialNodeType::Output;
			        });
			std::erase_if(m_graph->nodes, [&](const MaterialNode& n) { return std::ranges::find(selected, n.id) != selected.end(); });
			// Links to a removed node would otherwise dangle and re-emit on the next frame.
			std::erase_if(m_graph->links, [&](const MaterialLink& l)
			        {
				        return std::ranges::find(selected, l.fromNode) != selected.end()
				                || std::ranges::find(selected, l.toNode) != selected.end();
			        });
			ImNodes::ClearNodeSelection();
		}
	}

	void MaterialGraphPanel::DrawCanvas()
	{
		const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
		m_canvasOriginX = canvasMin.x + 60.0f;
		m_canvasOriginY = canvasMin.y + 60.0f;

		ApplyGraphTheme();
		if (!m_positionsApplied)
		{
			// The canvas pan is ImNodes' own state and survives switching material, so
			// opening a second graph showed wherever the first one had been scrolled to -
			// usually empty space, which reads exactly like a material with no graph.
			ImNodes::EditorContextResetPanning(ImVec2(40.0f, 40.0f));
		}
		ImNodes::BeginNodeEditor();

		for (MaterialNode& node: m_graph->nodes)
		{
			// Positions live in the material file so a layout survives a reload. Pushed once;
			// after that ImNodes owns them and dragging is written back below.
			if (!m_positionsApplied)
			{
				ImNodes::SetNodeGridSpacePos(node.id, ImVec2(node.x, node.y));
			}

			ImNodes::BeginNode(node.id);
			ImNodes::BeginNodeTitleBar();
			ImGui::TextUnformatted(MaterialNodeTypeName(node.type));
			ImNodes::EndNodeTitleBar();
			ImGui::SetItemTooltip("%s", MaterialNodeDescription(node.type));

			for (int pin = 0; pin < MaterialNodeInputCount(node.type); ++pin)
			{
				// An adaptive pin is drawn as the width it will actually resolve to, which is
				// the node's own output width - a Multiply of two float3s has float3 inputs.
				const MaterialValueType declared = MaterialNodeInputType(node.type, pin);
				const MaterialValueType shown = declared == MaterialValueType::Any ? MaterialNodeOutputType(m_graph.value(), node.id) : declared;
				ImNodes::PushColorStyle(ImNodesCol_Pin, TypeColour(shown));
				ImNodes::BeginInputAttribute(InputPinId(node.id, pin));
				ImGui::TextUnformatted(MaterialNodeInputName(node.type, pin));
				ImGui::SameLine();
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TypeColour(shown)), "%s", TypeSuffix(shown));
				ImGui::SetItemTooltip("%s", MaterialValueTypeName(shown));
				ImNodes::EndInputAttribute();
				ImNodes::PopColorStyle();
			}

			if (node.type != MaterialNodeType::Output)
			{
				ImGui::PushItemWidth(120.0f);
				ImGui::PushID(node.id);
				if (node.type == MaterialNodeType::ConstantColor)
				{
					ImGui::ColorEdit4("##c", node.value, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
				}
				else if (node.type == MaterialNodeType::ConstantFloat)
				{
					if (ImGui::DragFloat("##f", &node.value[0], 0.01f))
					{
						node.value[1] = node.value[0];
						node.value[2] = node.value[0];
					}
				}
				else if (node.type == MaterialNodeType::TextureSample || node.type == MaterialNodeType::NormalMap)
				{
					const char* slots[] = {"Albedo", "Normal", "Metal/Rough", "Occlusion", "Emissive"};
					int slot = static_cast<int>(node.slot);
					if (ImGui::Combo("##slot", &slot, slots, IM_ARRAYSIZE(slots)))
					{
						node.slot = static_cast<MaterialTextureSlot>(slot);
					}
					ImGui::SetItemTooltip("Which of the material's texture slots to read.\nThe texture itself is assigned in the list on the left.");
				}
				else if (node.type == MaterialNodeType::Panner)
				{
					ImGui::DragFloat2("##speed", node.value, 0.01f);
					ImGui::SetItemTooltip("UV units per second");
				}
				else if (node.type == MaterialNodeType::Noise)
				{
					ImGui::DragFloat("##scale", &node.value[0], 0.1f, 0.0f, 256.0f);
					ImGui::SetItemTooltip("Cells across the UV range");
				}
				else if (node.type == MaterialNodeType::Sine)
				{
					ImGui::DragFloat("##freq", &node.value[0], 0.05f);
					ImGui::SetItemTooltip("Frequency. With nothing plugged in, X is time.");
				}
				else if (node.type == MaterialNodeType::Remap)
				{
					ImGui::DragFloat2("##in", &node.value[0], 0.01f);
					ImGui::SetItemTooltip("From: min, max");
					ImGui::DragFloat2("##out", &node.value[2], 0.01f);
					ImGui::SetItemTooltip("To: min, max");
				}
				else if (node.type == MaterialNodeType::Channel)
				{
					const char* channels[] = {"R", "G", "B", "A"};
					ImGui::Combo("##ch", &node.channel, channels, IM_ARRAYSIZE(channels));
				}
				ImGui::PopID();
				ImGui::PopItemWidth();

				const MaterialValueType outType = MaterialNodeOutputType(m_graph.value(), node.id);
				ImNodes::PushColorStyle(ImNodesCol_Pin, TypeColour(outType));
				ImNodes::BeginOutputAttribute(OutputPinId(node.id));
				ImGui::TextUnformatted("Out");
				ImGui::SameLine();
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TypeColour(outType)), "%s", TypeSuffix(outType));
				ImGui::SetItemTooltip("%s", MaterialValueTypeName(outType));
				ImNodes::EndOutputAttribute();
				ImNodes::PopColorStyle();
			}
			ImNodes::EndNode();
		}

		for (const MaterialLink& link: m_graph->links)
		{
			const MaterialValueType from = MaterialNodeOutputType(*m_graph, link.fromNode);
			const MaterialValueType to = MaterialNodeInputType(m_graph->Find(link.toNode)->type, link.toPin);
			const MaterialConnection conversion = MaterialCanConnect(from, to);
			// A link that silently drops components is the one worth seeing: it is legal and
			// usually intended - a float4 colour into a float3 base colour - but it is also
			// how someone loses an alpha channel without noticing.
			const ImU32 colour = conversion == MaterialConnection::Truncate ? IM_COL32(200, 140, 90, 255) : TypeColour(from);
			ImNodes::PushColorStyle(ImNodesCol_Link, colour);
			ImNodes::Link(link.id, OutputPinId(link.fromNode), InputPinId(link.toNode, link.toPin));
			ImNodes::PopColorStyle();
		}

		ImNodes::MiniMap(0.18f, ImNodesMiniMapLocation_BottomRight);
		ImNodes::EndNodeEditor();
		m_positionsApplied = true;

		for (MaterialNode& node: m_graph->nodes)
		{
			const ImVec2 pos = ImNodes::GetNodeGridSpacePos(node.id);
			node.x = pos.x;
			node.y = pos.y;
		}

		SyncLinks();
		DeleteSelection();

		// Only REQUESTED here. A popup opened inside the canvas child but begun at the window
		// level never matches, and doing both inside the child put it under a child window
		// ImNodes had already claimed - either way the menu never appeared. One flag, and the
		// window level owns the popup.
		const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || (ImGui::IsKeyPressed(ImGuiKey_Space) && !ImGui::IsAnyItemActive())))
		{
			const ImVec2 mouse = ImGui::GetMousePos();
			m_addNodeScreenX = mouse.x;
			m_addNodeScreenY = mouse.y;
			m_addMenuRequested = true;
		}
	}

	void MaterialGraphPanel::DrawAddNodeMenu()
	{
		if (m_addMenuRequested)
		{
			m_addMenuRequested = false;
			m_addFilter[0] = '\0';
			m_addFilterFocus = true;
			ImGui::OpenPopup("##addnode");
		}

		if (!ImGui::BeginPopup("##addnode"))
		{
			return;
		}

		if (m_addFilterFocus)
		{
			// Typing immediately is the whole point; without this the box has to be clicked
			// first, which is slower than the menu it replaced.
			ImGui::SetKeyboardFocusHere();
			m_addFilterFocus = false;
		}
		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Search", m_addFilter, IM_ARRAYSIZE(m_addFilter));
		const bool filtering = m_addFilter[0] != '\0';

		const auto matches = [&](MaterialNodeType type)
		{
			if (!filtering)
			{
				return true;
			}
			// Case-insensitive on both the name and the description, so "rim" finds Fresnel
			// and "invert" finds One minus - the word someone reaches for is not always the
			// word the node ended up called.
			const auto contains = [&](std::string_view haystack)
			{
				const std::string_view needle(m_addFilter);
				return std::ranges::search(haystack, needle,
				               [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })
				               .begin()
				        != haystack.end();
			};
			return contains(MaterialNodeTypeName(type)) || contains(MaterialNodeDescription(type));
		};

		const auto place = [&](MaterialNodeType type)
		{
			MaterialNode node = MakeMaterialNode(m_graph->nextId++, type);
			// Placed where the menu was opened; the grid-space position is written back by
			// the canvas loop on the next frame.
			ImNodes::SetNodeScreenSpacePos(node.id, ImVec2(m_addNodeScreenX, m_addNodeScreenY));
			m_graph->nodes.push_back(node);
		};

		int shown = 0;
		MaterialNodeCategory heading = MaterialNodeCategory::Output;
		bool headingWritten = false;
		for (const MaterialNodeType type: MaterialAddableNodeTypes())
		{
			if (!matches(type))
			{
				continue;
			}
			// MaterialAddableNodeTypes is grouped by category, so a change of category is the
			// boundary between two sections. Suppressed while filtering: a search result is a
			// short list and the headings then outnumber the entries.
			if (!filtering && (!headingWritten || MaterialNodeCategoryOf(type) != heading))
			{
				heading = MaterialNodeCategoryOf(type);
				headingWritten = true;
				if (shown > 0)
				{
					ImGui::Separator();
				}
				ImGui::TextDisabled("%s", MaterialNodeCategoryName(heading));
			}
			++shown;

			if (ImGui::MenuItem(MaterialNodeTypeName(type)))
			{
				place(type);
			}
			ImGui::SetItemTooltip("%s", MaterialNodeDescription(type));
		}

		// Enter adds the only remaining match, so a search can be finished without the mouse.
		if (shown == 1 && ImGui::IsKeyPressed(ImGuiKey_Enter))
		{
			for (const MaterialNodeType type: MaterialAddableNodeTypes())
			{
				if (matches(type))
				{
					place(type);
					ImGui::CloseCurrentPopup();
					break;
				}
			}
		}
		if (shown == 0)
		{
			ImGui::TextDisabled("No node matches.");
		}
		ImGui::EndPopup();
	}

	bool MaterialGraphPanel::WriteMaterial(app::LayerContext& context)
	{
		if (m_path.empty() || !m_edit.loaded)
		{
			return false;
		}
		m_edit.spec.graphSection = GraphSignature();
		if (auto written = io::FileSystem::WriteFileText(m_path, MaterialSerializer::ToToml(m_edit.spec)); !written)
		{
			m_status = "Could not write the material.";
			m_statusIsError = true;
			return false;
		}
		// The property editor writes only when its copy differs from what is on disk, so
		// leaving `saved` behind here would make it write this material back out with the
		// shader path it had before the compile.
		m_edit.saved = m_edit.spec;
		m_edit.dirty = false;
		PropagateMaterialAsset(context, context.Get<World>(), m_path);
		return true;
	}

	void MaterialGraphPanel::RequestCompile(app::LayerContext& context)
	{
		// EditorProjectContext is the registered service; EditorProjectManager is not, and
		// asking for it returned null - which made every compile bail before doing anything,
		// silently, because this branch used to report nothing.
		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			m_status = "No project is open, so there is nowhere to write the shader.";
			m_statusIsError = true;
			return;
		}
		if (!m_graph || m_path.empty())
		{
			return;
		}
		if (m_compiling)
		{
			// One run at a time. The graph's current state is picked up when this one lands,
			// so dragging a slider queues exactly one more compile rather than one per frame.
			return;
		}

		std::string error;
		const std::string shader = GenerateMaterialShader(*m_graph, error);
		if (shader.empty())
		{
			m_status = error;
			m_statusIsError = true;
			// Marked as submitted so a graph that cannot generate is reported once instead of
			// being retried every frame.
			m_submittedSignature = GraphSignature();
			return;
		}

		const std::filesystem::path root = project->root;
		const std::string stem = MaterialStem(m_path);
		const std::filesystem::path slangPath = root / "assets" / "shaders" / (stem + ".slang");
		if (!io::file_util::CreateDirectories(slangPath.parent_path()) || !io::file_util::WriteText(slangPath, shader))
		{
			m_status = "Could not write the generated shader.";
			m_statusIsError = true;
			return;
		}

		PendingCompile pending;
		pending.materialPath = m_path;
		pending.signature = GraphSignature();
		pending.shaderVfsPath = std::format("shaders://{}.spv", stem);
		// The worker gets paths by value and touches nothing else on this object: the shader
		// source is already on disk, so slangc needs no access to the graph at all.
		const std::filesystem::path outDir = ProjectShaderIntermediateDir(root);
		auto* assets = context.TryGet<AssetManager>();
		// Interned HERE, on this thread: InternShaderVfsPath mutates a set the worker must
		// not touch. The view it returns is stable for the life of the AssetManager.
		const std::string_view internedShaderPath = assets != nullptr ? assets->InternShaderVfsPath(pending.shaderVfsPath) : std::string_view{};
		using PreparedList = std::vector<PipelineCache::PreparedReload>;
		pending.result = std::async(std::launch::async,
		        [slangPath, outDir, assets, internedShaderPath]() -> std::optional<PreparedList>
		        {
			        std::string compileError;
			        if (!CompileOne(slangPath, outDir, compileError))
			        {
				        // Reported through the log rather than carried back, so the worker
				        // owns no storage the panel might already have replaced.
				        AE_WARN(LogCategory::App, "Material graph: {}", compileError);
				        return std::nullopt;
			        }
			        // Building the shader objects is the other half of the stall: the driver
			        // compiles SPIR-V to machine code here, tens of milliseconds. Only the
			        // swap itself is left for the main thread.
			        if (assets == nullptr || internedShaderPath.empty())
			        {
				        return PreparedList{};
			        }
			        return assets->GetPipelineCache().PrepareReload(internedShaderPath);
		        });
		m_submittedSignature = pending.signature;
		m_compiling = std::move(pending);
		m_status = "Compiling...";
		m_statusIsError = false;
	}

	void MaterialGraphPanel::PollCompile(app::LayerContext& context)
	{
		if (!m_compiling)
		{
			return;
		}
		if (m_compiling->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			return;
		}

		PendingCompile finished = std::move(*m_compiling);
		m_compiling.reset();

		std::optional<std::vector<PipelineCache::PreparedReload>> built = finished.result.get();
		auto* assets = context.TryGet<AssetManager>();
		// Any path that abandons the result has to hand the shader objects back, or they are
		// leaked - they were created by the driver but never registered.
		const auto discard = [&]()
		{
			if (built && !built->empty() && assets != nullptr)
			{
				assets->GetPipelineCache().DiscardPrepared(std::move(*built));
			}
		};

		if (!built)
		{
			m_status = "Shader compile failed - see the Console. The previous shader is still in use.";
			m_statusIsError = true;
			return;
		}
		// The selection can move while a compile is in flight. Applying the result then would
		// write one material's shader path into another's file.
		if (finished.materialPath != m_path)
		{
			discard();
			return;
		}

		m_edit.spec.shaderVfsPath = finished.shaderVfsPath;
		if (!WriteMaterial(context))
		{
			discard();
			return;
		}

		// PipelineCache keys on the shader path, so recompiling to the same name used to
		// leave the pipeline built from the OLD bytes rendering - the edit compiled, the
		// material updated, and nothing on screen changed. Reload rebuilds that pipeline in
		// place and retires the previous one for a few frames.
		//
		// An earlier attempt pointed the preview at an ALTERNATING copy of the .spv to force
		// a new cache key. That is worse than it looks: the key comes back round on the third
		// compile and returns the pipeline built on the first, so the preview froze on an old
		// version - and looked correct exactly once, which is how it got committed.
		if (assets != nullptr && !built->empty())
		{
			assets->GetPipelineCache().CommitReload(std::move(*built));
		}

		m_status = std::format("Compiled {}", std::filesystem::path(m_path).filename().generic_string());
		m_statusIsError = false;
		m_compiledSignature = finished.signature;
		RefreshPreview(context);
	}

	void MaterialGraphPanel::RefreshPreview(app::LayerContext& context)
	{
		m_previewImGuiId = 0;
		m_previewError.clear();
		m_previewValid = false;
		m_previewSpec = m_edit.spec;

		auto* assets = context.TryGet<AssetManager>();
		auto* rendering = context.TryGet<RenderingSubsystem>();
		auto* imgui = context.TryGet<ImguiSubsystem>();
		auto* primitives = context.TryGet<PrimitiveMeshes>();
		if (assets == nullptr || rendering == nullptr || imgui == nullptr || primitives == nullptr)
		{
			m_previewError = "Preview is unavailable in this build.";
			return;
		}

		auto loaded = assets->LoadMaterialPreset(m_path);
		if (!loaded)
		{
			m_previewError = "Could not load the material for preview.";
			return;
		}

		// The file supplies the textures and the shader; the in-memory spec supplies the
		// values, so a slider moves the preview before anything has been saved.
		MaterialAsset material = *loaded;
		const MaterialAsset& edited = m_edit.spec.material;
		material.baseColorFactor = edited.baseColorFactor;
		material.metallicFactor = edited.metallicFactor;
		material.roughnessFactor = edited.roughnessFactor;
		material.occlusionStrength = edited.occlusionStrength;
		material.emissiveFactor = edited.emissiveFactor;
		material.alphaCutoff = edited.alphaCutoff;
		material.doubleSided = edited.doubleSided;
		material.alphaBlend = edited.alphaBlend;
		material.alphaMask = edited.alphaMask;
		material.modulateVertexColor = edited.modulateVertexColor;
		material.receiveShadows = edited.receiveShadows;

		std::string error;
		if (!rendering->GetModelPreview().ShowMaterialOnMesh(*assets, primitives->Get(m_previewMesh), material, error))
		{
			m_previewError = error;
		}
		else
		{
			const ImTextureID id = imgui->RegisterTexture(rendering->GetModelPreview().GetColorView(), gpu::ImageLayout::ShaderReadOnly);
			if (id != ImTextureID_Invalid)
			{
				m_previewImGuiId = static_cast<std::uint64_t>(id);
				m_previewValid = true;
			}
		}

		auto& textures = assets->GetTextureRegistry();
		for (const TextureHandle h: {loaded->albedoTex, loaded->normalTex, loaded->metallicRoughnessTex, loaded->occlusionTex, loaded->emissiveTex})
		{
			if (h.IsValid())
			{
				textures.Release(h);
			}
		}
	}

	void MaterialGraphPanel::DrawPreviewControls(app::LayerContext& context, const float width)
	{
		auto* rendering = context.TryGet<RenderingSubsystem>();
		if (rendering == nullptr)
		{
			return;
		}
		ModelPreviewService& preview = rendering->GetModelPreview();

		// A sphere shows curvature and a highlight; a plane is the only honest way to look at
		// a tiling texture; a cube shows how a normal map behaves across a hard edge.
		constexpr PrimitiveMesh kMeshes[] = {PrimitiveMesh::Sphere, PrimitiveMesh::Cube, PrimitiveMesh::Plane, PrimitiveMesh::Quad};
		const char* meshNames[] = {"Sphere", "Cube", "Plane", "Quad"};
		int meshIndex = 0;
		for (int i = 0; i < IM_ARRAYSIZE(kMeshes); ++i)
		{
			if (kMeshes[i] == m_previewMesh)
			{
				meshIndex = i;
			}
		}
		ImGui::SetNextItemWidth(width);
		if (ImGui::Combo("##mesh", &meshIndex, meshNames, IM_ARRAYSIZE(meshNames)))
		{
			m_previewMesh = kMeshes[meshIndex];
			RefreshPreview(context);
		}

		bool sceneSky = preview.IsSceneEnvironmentEnabled();
		if (ImGui::Checkbox("Scene sky", &sceneSky))
		{
			preview.SetSceneEnvironmentEnabled(sceneSky);
		}
		ImGui::SetItemTooltip("On: lit by the scene's sky, which is where the material will actually sit.\nOff: a neutral studio, so two materials can be compared.");

		ImGui::SameLine();
		bool spin = preview.IsTurntableEnabled();
		if (ImGui::Checkbox("Spin", &spin))
		{
			preview.SetTurntableEnabled(spin);
		}
		ImGui::SetItemTooltip("Stop the turntable to look at one angle.");
	}

	void MaterialGraphPanel::DrawPreview(const float side) const
	{
		if (m_previewImGuiId != 0)
		{
			ImGui::Image(static_cast<ImTextureID>(m_previewImGuiId), ImVec2(side, side));
		}
		else if (!m_previewError.empty())
		{
			ImGui::TextColored(chrome::kWarning, "%s", m_previewError.c_str());
		}
		else
		{
			ImGui::Dummy(ImVec2(side, side));
		}
	}

	void MaterialGraphPanel::Open(app::LayerContext& context, const std::string& materialPath)
	{
		// Switching away from unsaved edits would discard them with no undo and no warning,
		// which is what clicking another material in the browser used to do. Hold the request
		// and ask instead.
		if (m_edit.dirty && m_edit.loaded && !m_path.empty() && m_path != materialPath)
		{
			m_pendingOpenPath = materialPath;
			return;
		}
		m_pendingOpenPath.clear();
		m_path = materialPath;
		m_edit = MaterialAssetEditState{};
		m_edit.path = materialPath;
		m_graph.reset();
		m_status.clear();
		m_statusIsError = false;
		m_positionsApplied = false;
		m_compiledSignature.clear();
		m_previewImGuiId = 0;
		m_previewError.clear();
		m_previewValid = false;

		auto text = io::FileSystem::ReadFileText(materialPath);
		if (!text)
		{
			m_edit.error = "Could not read this material.";
			return;
		}
		// Not loaded when it does not parse: Parse hands back a DEFAULT spec, and WriteMaterial
		// would then save those defaults over the user's material.
		bool parsed = false;
		m_edit.spec = MaterialSerializer::Parse(materialPath, *text, &parsed);
		if (!parsed)
		{
			m_edit.error = "This material is not readable TOML; fix the file before editing its graph.";
			m_edit.spec = {};
			m_edit.loaded = false;
			return;
		}
		m_edit.loaded = true;

		// A graph that still lives in the old sidecar is folded into the material and the
		// sidecar removed. Leaving both is exactly the two-files-per-material problem the
		// single file exists to end, and they would drift the moment either was edited.
		if (const std::string legacy = LegacyGraphPath(materialPath); io::FileSystem::Exists(legacy))
		{
			if (auto sidecar = io::FileSystem::ReadFileText(legacy))
			{
				m_edit.spec.graphSection = MigrateLegacyGraphText(*sidecar);
				if (io::FileSystem::WriteFileText(materialPath, MaterialSerializer::ToToml(m_edit.spec)))
				{
					std::error_code ec;
					const std::filesystem::path physical = PhysicalPathFor(context, legacy);
					const bool removed = !physical.empty() && std::filesystem::remove(physical, ec);
					AE_INFO(LogCategory::App, "Material: folded '{}' into the material (sidecar removed: {})", legacy, removed);
				}
			}
		}
		m_edit.saved = m_edit.spec;

		m_graph = ParseMaterialGraph(MaterialSerializer::ToToml(m_edit.spec));
		m_compiledSignature = GraphSignature();
		m_submittedSignature = m_compiledSignature;
		RefreshPreview(context);
	}

	bool MaterialGraphPanel::SaveIfFocusedAndDirty(app::LayerContext& context)
	{
		if (!m_focused || !m_edit.loaded || !m_edit.dirty || m_path.empty())
		{
			return false;
		}
		return WriteMaterial(context);
	}

	void MaterialGraphPanel::DrawUnsavedSwitchPrompt(app::LayerContext& context)
	{
		if (m_pendingOpenPath.empty())
		{
			return;
		}
		ImGui::OpenPopup("Unsaved material##matSwitch");
		if (ImGui::BeginPopupModal("Unsaved material##matSwitch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const std::string current = std::filesystem::path(m_path).filename().generic_string();
			const std::string next = std::filesystem::path(m_pendingOpenPath).filename().generic_string();
			ImGui::Text("%s has unsaved changes.", current.c_str());
			ImGui::TextDisabled("Opening %s will lose them - a material file has no undo.", next.c_str());
			ImGui::Spacing();

			if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK "  Save and open", ImVec2(150.0f, 0.0f)))
			{
				const std::string next2 = m_pendingOpenPath;
				if (WriteMaterial(context))
				{
					m_pendingOpenPath.clear();
					m_edit.dirty = false;
					ImGui::CloseCurrentPopup();
					ImGui::EndPopup();
					Open(context, next2);
					return;
				}
				// Writing failed: stay put rather than lose the edits.
				m_pendingOpenPath.clear();
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Discard", ImVec2(110.0f, 0.0f)))
			{
				const std::string next2 = m_pendingOpenPath;
				m_pendingOpenPath.clear();
				m_edit.dirty = false;
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				Open(context, next2);
				return;
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Keep editing", ImVec2(130.0f, 0.0f)))
			{
				// The selection has moved on, but the edits are what matter.
				m_pendingOpenPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void MaterialGraphPanel::FollowSelection(app::LayerContext& context)
	{
		auto* selection = context.TryGet<SceneSelection>();
		if (selection == nullptr || !selection->HasAsset())
		{
			return;
		}
		const std::string& path = selection->SelectedAsset().path;

		// A leftover sidecar in the explorer opens the material it belongs to, which then
		// migrates it. Nothing is ever edited in the sidecar again.
		if (path.ends_with(".materialgraph.toml"))
		{
			const std::filesystem::path p(path);
			const std::string material = (p.parent_path() / (p.stem().stem().generic_string() + ".material.toml")).generic_string();
			if (io::FileSystem::Exists(material) && material != m_path)
			{
				Open(context, material);
			}
			return;
		}

		// Keyed on what the file IS, not on how it is named. A model import writes materials
		// with no .material.toml suffix at all, and keying on the suffix meant clicking one of
		// those left the window saying "nothing open" while the explorer called it a material.
		if (selection->SelectedAsset().kind != SceneSelection::AssetKind::Material)
		{
			return;
		}
		if (path != m_path)
		{
			Open(context, path);
		}
	}

	void MaterialGraphPanel::DrawToolbar(app::LayerContext& context)
	{
		ImGui::TextUnformatted(std::filesystem::path(m_path).filename().generic_string().c_str());
		ImGui::SetItemTooltip("%s", m_path.c_str());
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
		{
			if (WriteMaterial(context))
			{
				m_status = "Saved.";
				m_statusIsError = false;
			}
		}

		if (!m_graph)
		{
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_DIAGRAM_PROJECT "  Add a node graph"))
			{
				m_graph = GraphFromMaterial(m_edit.spec);
				m_positionsApplied = false;
				m_compiledSignature.clear();
				m_submittedSignature.clear();
				m_status = "Graph created from this material's values and textures.";
				m_statusIsError = false;
			}
			ImGui::SetItemTooltip("Generate this material's shader from nodes.\nSeeded from what it already is, so nothing about it changes.");
			return;
		}

		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_HAMMER "  Compile"))
		{
			RequestCompile(context);
		}
		ImGui::SameLine();
		ImGui::Checkbox("Auto", &m_autoCompile);
		ImGui::SetItemTooltip("Recompile shortly after the graph stops changing");
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_PLUS "  Add node"))
		{
			m_addNodeScreenX = m_canvasOriginX;
			m_addNodeScreenY = m_canvasOriginY;
			m_addMenuRequested = true;
		}
		ImGui::SetItemTooltip("Right-click the canvas, or press space, to add one where the cursor is.");

		// Adding a graph must not be a one-way door: the graph lives in the material now, so
		// without this the only way back to a plain material is editing the file by hand.
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_TRASH "  Remove graph"))
		{
			ImGui::OpenPopup("##removegraph");
		}
		ImGui::SetItemTooltip("Go back to a plain material driven by the values on the left.");
		if (ImGui::BeginPopup("##removegraph"))
		{
			ImGui::TextUnformatted("Delete this material's node graph?");
			ImGui::TextDisabled("The material keeps its values and textures, and goes back to the standard shader.");
			if (ImGui::Button("Delete"))
			{
				m_graph.reset();
				// Back to the engine's standard shader: leaving the generated one would keep
				// rendering a shader nothing can edit any more.
				m_edit.spec.shaderVfsPath.clear();
				m_compiledSignature.clear();
				m_submittedSignature.clear();
				WriteMaterial(context);
				RefreshPreview(context);
				m_status = "Graph removed.";
				m_statusIsError = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void MaterialGraphPanel::DrawSidebar(app::LayerContext& context, const float width)
	{
		// A child rather than a group: a group is only as wide as its widest item, so one
		// control that asked for the remaining width made the column swallow the canvas.
		ImGui::BeginChild("##sidebar", ImVec2(width, 0.0f));
		DrawPreview(width);
		DrawPreviewControls(context, width);
		ImGui::Separator();
		DrawMaterialAssetEditor(context, context.Get<World>(), m_path, m_edit);
		ImGui::EndChild();
	}

	void MaterialGraphPanel::OnImGui(app::LayerContext& context)
	{
		ImGui::Begin("Material", VisiblePtr());
		m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		PollCompile(context);
		FollowSelection(context);
		// Drawn before the early return below, or a pending switch would be unanswerable
		// whenever the window is showing its empty state.
		DrawUnsavedSwitchPrompt(context);

		if (m_path.empty())
		{
			ImGui::TextDisabled("Nothing open.");
			ImGui::TextDisabled("Select a material in the File Explorer.");
			ImGui::End();
			return;
		}

		if (!m_edit.error.empty())
		{
			ImGui::TextColored(chrome::kWarning, "%s", m_edit.error.c_str());
			ImGui::End();
			return;
		}

		if (m_path.ends_with(".material"))
		{
			// The importer's cooked binary. Editing it would be overwritten by the next model
			// import, and there is no authored source to edit instead.
			ImGui::TextUnformatted(std::filesystem::path(m_path).filename().generic_string().c_str());
			ImGui::TextDisabled("Imported with a model - not editable.");
			ImGui::End();
			return;
		}

		DrawToolbar(context);
		if (!m_status.empty())
		{
			ImGui::TextColored(m_statusIsError ? chrome::kWarning : chrome::kSuccess, "%s", m_status.c_str());
		}

		// The preview follows the values being edited, so it is rebuilt whenever they differ
		// from what it was last built with rather than only when the file is written.
		if (!MaterialSpecEquals(m_edit.spec, m_previewSpec))
		{
			RefreshPreview(context);
		}

		// Clamped rather than fixed: the window can be docked narrow, and a sidebar wider than
		// the window would push the canvas out of it entirely - which is exactly what a
		// hard-coded width did once already.
		const float available = ImGui::GetContentRegionAvail().x;
		m_sidebarWidth = std::clamp(m_sidebarWidth, 220.0f, std::max(220.0f, available - 220.0f));
		DrawSidebar(context, m_sidebarWidth);

		if (m_graph)
		{
			ImGui::SameLine();
			// The splitter. An invisible button is the standard idiom - ImGui has no splitter
			// widget - drawn over so it is visible, and only highlighted while it is in play.
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
			const ImVec2 gripMin = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##split", ImVec2(6.0f, ImGui::GetContentRegionAvail().y));
			const bool gripActive = ImGui::IsItemActive();
			if (gripActive)
			{
				m_sidebarWidth += ImGui::GetIO().MouseDelta.x;
			}
			if (gripActive || ImGui::IsItemHovered())
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
			}
			const ImVec2 gripMax = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(gripMin.x + 2.0f, gripMin.y), ImVec2(gripMax.x - 2.0f, gripMax.y),
			        ImGui::GetColorU32(gripActive ? ImGuiCol_SeparatorActive : (ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator)));
			ImGui::PopStyleVar();
			ImGui::SameLine();

			ImGui::BeginChild("##canvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			DrawCanvas();
			ImGui::EndChild();

			// Submitted at the WINDOW level, outside the canvas child - see DrawCanvas.
			DrawAddNodeMenu();

			// Compiling shells out to slangc, so it waits for the graph to settle. Comparing
			// the serialised graph is what makes "changed" mean changed rather than "a frame
			// passed".
			if (m_autoCompile)
			{
				// Compared against what was SUBMITTED, not what last succeeded: a graph that
				// fails to compile would otherwise be resubmitted every frame forever.
				if (GraphSignature() != m_submittedSignature)
				{
					m_idleSeconds += ImGui::GetIO().DeltaTime;
					if (m_idleSeconds >= kAutoCompileIdleSeconds && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
					{
						m_idleSeconds = 0.0f;
						RequestCompile(context);
					}
				}
				else
				{
					m_idleSeconds = 0.0f;
				}
			}
		}
		ImGui::End();
	}
} // namespace aether::editor
