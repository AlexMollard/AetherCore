#include "debug/MaterialGraphPanel.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
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
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/World.hpp"

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

		// Compiling runs slangc as a subprocess, so it waits for a pause rather than firing on
		// every mouse-move of a slider.
		constexpr float kAutoCompileIdleSeconds = 0.35f;

		std::string GraphStem(const std::string& graphPath)
		{
			return std::filesystem::path(graphPath).stem().stem().generic_string();
		}

		// The material a graph drives. Written next to the graph so compiling produces
		// something that can actually be dropped on an object, rather than an instruction to
		// go and make one by hand.
		std::string CompanionMaterialPath(const std::string& graphPath)
		{
			const std::filesystem::path p(graphPath);
			return (p.parent_path() / (GraphStem(graphPath) + ".material.toml")).generic_string();
		}

		// The reverse: the graph that generated a material, if there is one.
		std::string GraphForMaterial(const std::string& materialPath)
		{
			const std::filesystem::path p(materialPath);
			const std::string stem = std::filesystem::path(materialPath).stem().stem().generic_string();
			return (p.parent_path() / (stem + ".materialgraph.toml")).generic_string();
		}

		// Seeds a graph from a material's current values, so moving an existing material onto
		// the node editor starts from what it already looks like rather than from grey.
		MaterialGraph GraphFromMaterial(const MaterialPresetSpec& spec)
		{
			MaterialGraph graph;
			MaterialNode out{.id = 1, .type = MaterialNodeType::Output, .x = 460.0f, .y = 160.0f};
			MaterialNode colour{.id = 2, .type = MaterialNodeType::ConstantColor, .x = 80.0f, .y = 80.0f};
			MaterialNode metallic{.id = 3, .type = MaterialNodeType::ConstantFloat, .x = 80.0f, .y = 230.0f};
			MaterialNode roughness{.id = 4, .type = MaterialNodeType::ConstantFloat, .x = 80.0f, .y = 330.0f};
			for (int i = 0; i < 4; ++i)
			{
				colour.value[i] = spec.material.baseColorFactor[i];
			}
			metallic.value[0] = metallic.value[1] = metallic.value[2] = spec.material.metallicFactor;
			roughness.value[0] = roughness.value[1] = roughness.value[2] = spec.material.roughnessFactor;
			graph.nodes = {out, colour, metallic, roughness};
			graph.links = {
			        MaterialLink{.id = 5, .fromNode = 2, .fromPin = 0, .toNode = 1, .toPin = 0},
			        MaterialLink{.id = 6, .fromNode = 3, .fromPin = 0, .toNode = 1, .toPin = 1},
			        MaterialLink{.id = 7, .fromNode = 4, .fromPin = 0, .toNode = 1, .toPin = 2}};
			graph.nextId = 8;
			return graph;
		}
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
			std::erase_if(m_graph.links, [&](const MaterialLink& l) { return std::ranges::find(selected, l.id) != selected.end(); });
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
				        const MaterialNode* node = m_graph.Find(id);
				        return node == nullptr || node->type == MaterialNodeType::Output;
			        });
			std::erase_if(m_graph.nodes, [&](const MaterialNode& n) { return std::ranges::find(selected, n.id) != selected.end(); });
			// Links to a removed node would otherwise dangle and re-emit on the next frame.
			std::erase_if(m_graph.links, [&](const MaterialLink& l)
			        {
				        return std::ranges::find(selected, l.fromNode) != selected.end()
				                || std::ranges::find(selected, l.toNode) != selected.end();
			        });
			ImNodes::ClearNodeSelection();
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

			for (int pin = 0; pin < MaterialNodeInputCount(node.type); ++pin)
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

		ImNodes::MiniMap(0.18f, ImNodesMiniMapLocation_BottomRight);
		ImNodes::EndNodeEditor();
		m_positionsApplied = true;

		for (MaterialNode& node: m_graph.nodes)
		{
			const ImVec2 pos = ImNodes::GetNodeGridSpacePos(node.id);
			node.x = pos.x;
			node.y = pos.y;
		}

		SyncLinks();
		DeleteSelection();

		// BeginPopupContextItem keys off the LAST submitted item, which after EndNodeEditor is
		// whatever ImNodes drew last rather than the canvas - so the add menu never opened.
		// Asking ImNodes whether the editor itself is hovered is the actual question.
		if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			const ImVec2 mouse = ImGui::GetMousePos();
			m_addNodeScreenX = mouse.x;
			m_addNodeScreenY = mouse.y;
			ImGui::OpenPopup("##addnode");
		}
	}

	bool MaterialGraphPanel::Compile(app::LayerContext& context)
	{
		// EditorProjectContext is the registered service; EditorProjectManager is not, and
		// asking for it returned null - which made every compile bail before doing anything,
		// silently, because this branch used to report nothing.
		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			m_status = "No project is open, so there is nowhere to write the shader.";
			m_statusIsError = true;
			return false;
		}
		if (m_graphPath.empty())
		{
			return false;
		}

		std::string error;
		const std::string shader = GenerateMaterialShader(m_graph, error);
		if (shader.empty())
		{
			m_status = error;
			m_statusIsError = true;
			return false;
		}

		const std::filesystem::path root = project->root;
		const std::string stem = GraphStem(m_graphPath);
		const std::filesystem::path slangPath = root / "assets" / "shaders" / (stem + ".slang");
		if (!io::file_util::CreateDirectories(slangPath.parent_path()) || !io::file_util::WriteText(slangPath, shader))
		{
			m_status = "Could not write the generated shader.";
			m_statusIsError = true;
			return false;
		}

		// The same compiler the project shaders go through, so a graph result is an ordinary
		// project shader from here on and nothing downstream has to know better.
		std::string compileError;
		if (!CompileOne(slangPath, ProjectShaderIntermediateDir(root), compileError))
		{
			m_status = compileError;
			m_statusIsError = true;
			return false;
		}

		// A material to carry the shader, written once and then left alone: the texture slots
		// and factors on it are the author's, and recompiling a graph must not reset them.
		const std::string materialPath = CompanionMaterialPath(m_graphPath);
		MaterialPresetSpec spec;
		if (auto existing = io::FileSystem::ReadFileText(materialPath))
		{
			spec = MaterialSerializer::Parse(materialPath, *existing);
		}
		spec.shaderVfsPath = std::format("shaders://{}.spv", stem);
		if (auto written = io::FileSystem::WriteFileText(materialPath, MaterialSerializer::ToToml(spec)); !written)
		{
			m_status = "Compiled, but could not write the material beside the graph.";
			m_statusIsError = true;
			return false;
		}

		// PipelineCache keys on the shader PATH and has no invalidation, so recompiling to the
		// same name leaves the old pipeline - and therefore the old shader - rendering. The
		// committed material keeps the stable name so a fresh clone resolves it, and the
		// preview points at an alternating copy in build output purely to force a new key.
		//
		// Alternating rather than incrementing bounds the cache at two pipelines per graph.
		// Dropping and rebuilding the real one would mean destroying a pipeline that may be in
		// flight, and a vkDeviceWaitIdle from this thread is the cross-queue bug the settings
		// work already ran into once.
		m_previewGeneration = 1 - m_previewGeneration;
		const std::filesystem::path builtSpv = ProjectShaderIntermediateDir(root) / (stem + ".spv");
		const std::filesystem::path previewSpv = ProjectShaderIntermediateDir(root) / std::format("{}.preview{}.spv", stem, m_previewGeneration);
		// std::filesystem rather than io::file_util::CopyFile: windows.h defines CopyFile as
		// an object-like macro for CopyFileA, which rewrites the name even when it is
		// qualified, so the call never resolves.
		std::string previewShaderPath;
		std::error_code copyEc;
		std::filesystem::copy_file(builtSpv, previewSpv, std::filesystem::copy_options::overwrite_existing, copyEc);
		if (!copyEc)
		{
			previewShaderPath = std::format("shaders://{}.preview{}.spv", stem, m_previewGeneration);
		}

		m_status = std::format("Compiled to {}.material.toml", stem);
		m_statusIsError = false;
		m_compiledSignature = SerializeMaterialGraph(m_graph);
		RefreshPreview(context, materialPath, previewShaderPath);
		return true;
	}

	void MaterialGraphPanel::RefreshPreview(app::LayerContext& context, const std::string& materialPath, const std::string& shaderOverride)
	{
		m_previewDirty = false;
		m_previewImGuiId = 0;
		m_previewError.clear();
		m_previewMaterialPath = materialPath;

		auto* assets = context.TryGet<AssetManager>();
		auto* rendering = context.TryGet<RenderingSubsystem>();
		auto* imgui = context.TryGet<ImguiSubsystem>();
		auto* primitives = context.TryGet<PrimitiveMeshes>();
		if (assets == nullptr || rendering == nullptr || imgui == nullptr || primitives == nullptr)
		{
			m_previewError = "Preview is unavailable in this build.";
			return;
		}

		auto loaded = assets->LoadMaterialPreset(materialPath);
		if (!loaded)
		{
			m_previewError = "Could not load the material for preview.";
			return;
		}

		MaterialAsset material = *loaded;
		if (!shaderOverride.empty())
		{
			material.templateDesc.shaderVfsPath = assets->InternShaderVfsPath(shaderOverride);
		}

		std::string error;
		if (!rendering->GetModelPreview().ShowMaterialOnMesh(*assets, primitives->Get(PrimitiveMesh::Sphere), material, error))
		{
			m_previewError = error;
		}
		else
		{
			const ImTextureID id = imgui->RegisterTexture(rendering->GetModelPreview().GetColorView(), gpu::ImageLayout::ShaderReadOnly);
			if (id != ImTextureID_Invalid)
			{
				m_previewImGuiId = static_cast<std::uint64_t>(id);
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

	void MaterialGraphPanel::DrawPreviewControls(app::LayerContext& context)
	{
		auto* rendering = context.TryGet<RenderingSubsystem>();
		if (rendering == nullptr)
		{
			return;
		}
		bool sceneSky = rendering->GetModelPreview().IsSceneEnvironmentEnabled();
		if (ImGui::Checkbox("Scene sky", &sceneSky))
		{
			rendering->GetModelPreview().SetSceneEnvironmentEnabled(sceneSky);
		}
		ImGui::SetItemTooltip("On: lit by the scene's sky, which is where the material will actually sit.\nOff: a neutral studio, so two materials can be compared.");
	}

	void MaterialGraphPanel::DrawPreview(float side) const
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

	void MaterialGraphPanel::FollowSelection(app::LayerContext& context)
	{
		auto* selection = context.TryGet<SceneSelection>();
		if (selection == nullptr || !selection->HasAsset())
		{
			return;
		}
		const std::string& path = selection->SelectedAsset().path;

		if (path.ends_with(".materialgraph.toml") && path != m_graphPath)
		{
			m_graphPath = path;
			m_edit = MaterialAssetEditState{};
			if (auto text = io::FileSystem::ReadFileText(path))
			{
				m_graph = ParseMaterialGraph(*text);
			}
			m_positionsApplied = false;
			m_compiledSignature.clear();
			m_status.clear();
			// Show whatever the graph last compiled to, and otherwise show nothing rather
			// than leaving the previous material's sphere on screen looking like this one.
			if (io::FileSystem::Exists(CompanionMaterialPath(path)))
			{
				RefreshPreview(context, CompanionMaterialPath(path));
			}
			else
			{
				m_previewImGuiId = 0;
				m_previewError.clear();
				m_previewMaterialPath.clear();
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

		// A material that HAS a graph is always shown as its graph. Without this, whether you
		// got the node editor or the property list depended on what you had clicked before it:
		// the graph's own material kept the graph open, but arriving from another material
		// dropped you into the property editor for the same file.
		if (const std::string graphPath = GraphForMaterial(path); io::FileSystem::Exists(graphPath))
		{
			if (graphPath != m_graphPath)
			{
				m_graphPath = graphPath;
				m_edit = MaterialAssetEditState{};
				if (auto text = io::FileSystem::ReadFileText(graphPath))
				{
					m_graph = ParseMaterialGraph(*text);
				}
				m_positionsApplied = false;
				m_compiledSignature.clear();
				m_status.clear();
				RefreshPreview(context, path);
			}
			return;
		}

		if (path != m_edit.path)
		{
			m_graphPath.clear();
			m_edit = MaterialAssetEditState{};
			m_edit.path = path;
			m_previewDirty = true;
		}
	}

	void MaterialGraphPanel::DrawGraphMode(app::LayerContext& context)
	{
		ImGui::TextDisabled("%s", std::filesystem::path(m_graphPath).filename().generic_string().c_str());
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
		{
			if (auto written = io::FileSystem::WriteFileText(m_graphPath, SerializeMaterialGraph(m_graph)); !written)
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
		ImGui::SameLine();
		ImGui::Checkbox("Auto", &m_autoCompile);
		ImGui::SetItemTooltip("Recompile shortly after the graph stops changing");
		ImGui::SameLine();
		ImGui::TextDisabled("right-click to add  |  Del removes");
		ImGui::SameLine();
		ImGui::TextDisabled("-> %s", std::filesystem::path(CompanionMaterialPath(m_graphPath)).filename().generic_string().c_str());
		ImGui::SetItemTooltip("The material this graph writes. Drop it on an object to use the graph.");

		if (!m_status.empty())
		{
			ImGui::TextColored(m_statusIsError ? chrome::kWarning : chrome::kSuccess, "%s", m_status.c_str());
		}

		// The preview sits beside the canvas rather than above it: a node graph wants the
		// height, and the whole point is watching the sphere while wiring.
		const float previewSide = std::min(220.0f, ImGui::GetContentRegionAvail().x * 0.35f);
		ImGui::BeginGroup();
		DrawPreview(previewSide);
		DrawPreviewControls(context);
		ImGui::EndGroup();
		ImGui::SameLine();

		ImGui::BeginChild("##canvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		DrawCanvas();

		if (ImGui::BeginPopup("##addnode"))
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
					// Placed where the menu was opened; the grid-space position is written
					// back by the canvas loop on the next frame.
					ImNodes::SetNodeScreenSpacePos(node.id, ImVec2(m_addNodeScreenX, m_addNodeScreenY));
					m_graph.nodes.push_back(node);
				}
			}
			ImGui::EndPopup();
		}
		ImGui::EndChild();

		// Compiling shells out to slangc, so it waits for the graph to settle. Comparing the
		// serialised graph is what makes "changed" mean changed rather than "a frame passed".
		if (m_autoCompile)
		{
			const std::string signature = SerializeMaterialGraph(m_graph);
			if (signature != m_compiledSignature)
			{
				m_idleSeconds += ImGui::GetIO().DeltaTime;
				if (m_idleSeconds >= kAutoCompileIdleSeconds && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
				{
					m_idleSeconds = 0.0f;
					Compile(context);
				}
			}
			else
			{
				m_idleSeconds = 0.0f;
			}
		}
	}

	void MaterialGraphPanel::OnImGui(app::LayerContext& context)
	{
		ImGui::Begin("Material", VisiblePtr());
		FollowSelection(context);

		if (!m_graphPath.empty())
		{
			DrawGraphMode(context);
			ImGui::End();
			return;
		}

		if (m_edit.path.empty())
		{
			ImGui::TextDisabled("Nothing open.");
			ImGui::TextDisabled("Select a .material.toml or a .materialgraph.toml in the File Explorer.");
			ImGui::End();
			return;
		}

		ImGui::TextUnformatted(std::filesystem::path(m_edit.path).filename().generic_string().c_str());
		ImGui::SetItemTooltip("%s", m_edit.path.c_str());
		ImGui::Separator();

		if (m_edit.path.ends_with(".material"))
		{
			// The importer's cooked binary. Editing it would be overwritten by the next model
			// import, and there is no authored source to edit instead.
			ImGui::TextDisabled("Imported with a model - not editable.");
			ImGui::End();
			return;
		}

		// The preview follows the values being edited, so it has to be rebuilt whenever they
		// differ from what it was last built with rather than only when the file is written.
		if (m_previewDirty || m_previewMaterialPath != m_edit.path || !MaterialSpecEquals(m_edit.spec, m_previewSpec))
		{
			RefreshPreview(context, m_edit.path);
			m_previewSpec = m_edit.spec;
		}
		DrawPreview(std::min(220.0f, ImGui::GetContentRegionAvail().x));
		DrawPreviewControls(context);

		// The way out of the property editor and onto the node graph, seeded from what this
		// material already is so the conversion changes nothing about how it looks.
		if (ImGui::Button(ICON_FA_DIAGRAM_PROJECT "  Convert to graph"))
		{
			const std::string graphPath = GraphForMaterial(m_edit.path);
			const MaterialGraph seeded = GraphFromMaterial(m_edit.spec);
			if (auto written = io::FileSystem::WriteFileText(graphPath, SerializeMaterialGraph(seeded)); written)
			{
				m_graph = seeded;
				m_graphPath = graphPath;
				m_positionsApplied = false;
				m_compiledSignature.clear();
				m_status = "Converted. The graph now drives this material.";
				m_statusIsError = false;
			}
		}
		ImGui::SetItemTooltip("Start a node graph from these values. The material keeps its name and everything using it.");
		ImGui::Separator();

		DrawMaterialAssetEditor(context, context.Get<World>(), m_edit.path, m_edit);
		ImGui::End();
	}
} // namespace aether::editor
