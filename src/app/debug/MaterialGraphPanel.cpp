#include "debug/MaterialGraphPanel.hpp"

#include <algorithm>
#include <filesystem>
#include <format>

#include <imgui.h>
#include <imnodes.h>

#include "debug/EditorChrome.hpp"
#include "debug/EditorProjectManager.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "editor/ShaderCompiler.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "layers/AppLayer.hpp"

namespace aether::editor
{
	namespace
	{
		// ImNodes addresses pins by a flat integer id, so node and pin are packed into one.
		// A stride of 16 leaves room for the four Output inputs plus an output pin, and keeps
		// pin ids from aliasing onto node ids.
		constexpr int kPinStride = 16;
		constexpr int kOutputPinSlot = 8;

		int InputPinId(int nodeId, int pin) { return nodeId * kPinStride + pin; }
		int OutputPinId(int nodeId) { return nodeId * kPinStride + kOutputPinSlot; }
		int NodeOfPin(int pinId) { return pinId / kPinStride; }
		bool IsOutputPin(int pinId) { return (pinId % kPinStride) == kOutputPinSlot; }
		int PinIndex(int pinId) { return pinId % kPinStride; }
	} // namespace

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
				const int toNode = NodeOfPin(endPin);
				const int toPin = PinIndex(endPin);
				// An input takes one link; connecting again replaces rather than stacks.
				std::erase_if(m_graph.links, [&](const MaterialLink& l) { return l.toNode == toNode && l.toPin == toPin; });
				m_graph.links.push_back(MaterialLink{
				        .id = m_graph.nextId++, .fromNode = NodeOfPin(startPin), .fromPin = 0, .toNode = toNode, .toPin = toPin});
			}
		}

		int destroyed = 0;
		if (ImNodes::IsLinkDestroyed(&destroyed))
		{
			std::erase_if(m_graph.links, [&](const MaterialLink& l) { return l.id == destroyed; });
		}
	}

	void MaterialGraphPanel::DrawCanvas()
	{
		ImNodes::BeginNodeEditor();

		for (MaterialNode& node: m_graph.nodes)
		{
			// Positions live in the graph file so a layout survives a reload. Pushed once;
			// after that ImNodes owns them and dragging is written back below.
			if (!m_positionsApplied)
			{
				ImNodes::SetNodeGridSpacePos(node.id, ImVec2(node.x, node.y));
			}

			ImNodes::BeginNode(node.id);
			ImNodes::BeginNodeTitleBar();
			ImGui::TextUnformatted(MaterialNodeTypeName(node.type));
			ImNodes::EndNodeTitleBar();

			const int inputs = MaterialNodeInputCount(node.type);
			for (int pin = 0; pin < inputs; ++pin)
			{
				ImNodes::BeginInputAttribute(InputPinId(node.id, pin));
				ImGui::TextUnformatted(MaterialNodeInputName(node.type, pin));
				ImNodes::EndInputAttribute();
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
				ImGui::PopID();
				ImGui::PopItemWidth();

				ImNodes::BeginOutputAttribute(OutputPinId(node.id));
				ImGui::TextUnformatted("Out");
				ImNodes::EndOutputAttribute();
			}
			ImNodes::EndNode();
		}

		for (const MaterialLink& link: m_graph.links)
		{
			ImNodes::Link(link.id, OutputPinId(link.fromNode), InputPinId(link.toNode, link.toPin));
		}

		ImNodes::EndNodeEditor();
		m_positionsApplied = true;

		for (MaterialNode& node: m_graph.nodes)
		{
			const ImVec2 pos = ImNodes::GetNodeGridSpacePos(node.id);
			node.x = pos.x;
			node.y = pos.y;
		}

		SyncLinks();
	}

	void MaterialGraphPanel::Compile(app::LayerContext& context)
	{
		auto* projects = context.TryGet<EditorProjectManager>();
		if (projects == nullptr || !projects->CurrentProject().IsLoaded())
		{
			m_status = "Open a project first.";
			m_statusIsError = true;
			return;
		}
		if (m_path.empty())
		{
			m_status = "Select a .materialgraph.toml in the File Explorer first.";
			m_statusIsError = true;
			return;
		}

		std::string error;
		const std::string shader = GenerateMaterialShader(m_graph, error);
		if (shader.empty())
		{
			m_status = error;
			m_statusIsError = true;
			return;
		}

		const std::filesystem::path root = projects->CurrentProject().root;
		const std::string stem = std::filesystem::path(m_path).stem().stem().generic_string();
		const std::filesystem::path slangPath = root / "assets" / "shaders" / (stem + ".slang");
		if (auto dirs = io::file_util::CreateDirectories(slangPath.parent_path()); !dirs)
		{
			m_status = "Could not create the shader folder.";
			m_statusIsError = true;
			return;
		}
		if (auto written = io::file_util::WriteText(slangPath, shader); !written)
		{
			m_status = "Could not write the generated shader.";
			m_statusIsError = true;
			return;
		}

		// The same compiler the project shaders go through, so a graph result is an ordinary
		// project shader from here on and nothing downstream has to know better.
		std::string compileError;
		if (!CompileOne(slangPath, ProjectShaderIntermediateDir(root), compileError))
		{
			m_status = compileError;
			m_statusIsError = true;
			return;
		}

		m_status = std::format("Compiled. Set a material shader to shaders://{}.spv", stem);
		m_statusIsError = false;
	}

	void MaterialGraphPanel::DrawToolbar(app::LayerContext& context)
	{
		auto* selection = context.TryGet<SceneSelection>();
		if (selection != nullptr && selection->HasAsset()
		        && selection->SelectedAsset().path.ends_with(".materialgraph.toml")
		        && selection->SelectedAsset().path != m_path)
		{
			m_path = selection->SelectedAsset().path;
			if (auto text = io::FileSystem::ReadFileText(m_path))
			{
				m_graph = ParseMaterialGraph(*text);
			}
			m_positionsApplied = false;
			m_status.clear();
		}

		ImGui::TextDisabled("%s", m_path.empty() ? "no graph open" : std::filesystem::path(m_path).filename().generic_string().c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(m_path.empty());
		if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
		{
			if (auto written = io::FileSystem::WriteFileText(m_path, SerializeMaterialGraph(m_graph)); !written)
			{
				m_status = "Could not write the graph.";
				m_statusIsError = true;
			}
			else
			{
				m_status = "Graph saved.";
				m_statusIsError = false;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_HAMMER "  Compile"))
		{
			Compile(context);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("right-click the canvas to add a node");

		if (!m_status.empty())
		{
			ImGui::TextColored(m_statusIsError ? chrome::kWarning : chrome::kSuccess, "%s", m_status.c_str());
		}
	}

	void MaterialGraphPanel::OnImGui(app::LayerContext& context)
	{
		ImGui::Begin("Material Graph", VisiblePtr());
		DrawToolbar(context);
		DrawCanvas();

		if (ImGui::BeginPopupContextItem("##addnode"))
		{
			const MaterialNodeType addable[] = {
			        MaterialNodeType::ConstantColor, MaterialNodeType::ConstantFloat, MaterialNodeType::TextureSample,
			        MaterialNodeType::Uv, MaterialNodeType::Time, MaterialNodeType::Fresnel,
			        MaterialNodeType::Multiply, MaterialNodeType::Add, MaterialNodeType::Lerp,
			        MaterialNodeType::NormalMap, MaterialNodeType::Panner, MaterialNodeType::Noise,
			        MaterialNodeType::Step};
			for (const MaterialNodeType type: addable)
			{
				if (ImGui::MenuItem(MaterialNodeTypeName(type)))
				{
					MaterialNode node{.id = m_graph.nextId++, .type = type, .x = 60.0f, .y = 60.0f};
					// The all-ones default reads as a 1 cell/second panner and a 1-cell
					// noise, neither of which shows anything useful on first drop.
					if (type == MaterialNodeType::Panner)
					{
						node.value[0] = 0.1f;
						node.value[1] = 0.0f;
					}
					else if (type == MaterialNodeType::Noise)
					{
						node.value[0] = 8.0f;
					}
					else if (type == MaterialNodeType::NormalMap)
					{
						node.slot = MaterialTextureSlot::Normal;
					}
					ImNodes::SetNodeGridSpacePos(node.id, ImVec2(node.x, node.y));
					m_graph.nodes.push_back(node);
				}
			}
			ImGui::EndPopup();
		}

		ImGui::End();
	}
} // namespace aether::editor
