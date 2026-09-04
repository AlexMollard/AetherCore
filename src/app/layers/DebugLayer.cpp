#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <span>
#include <vector>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#endif

using namespace std::string_view_literals;

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "debug/EditorShortcuts.hpp"
#include "debug/ConsolePanel.hpp"
#include "debug/OpenInEditor.hpp"
#include "debug/ScriptErrorOverlay.hpp"
#include "debug/ControlServerPanel.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/DevToolsPanel.hpp"
#include "debug/FileExplorerPanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "debug/HierarchyPanel.hpp"
#include "debug/InspectorPanel.hpp"
#include "debug/MaterialGraphPanel.hpp"
#include "debug/PerformancePanel.hpp"
#include "debug/BuildPanel.hpp"
#include "debug/ProjectPanel.hpp"
#include "debug/RenderGraphPanel.hpp"
#include "debug/SettingsPanel.hpp"
#include "debug/ThemePanel.hpp"
#include "debug/PostProcessingPanel.hpp"
#include "debug/TextureInspectorPanel.hpp"
#include "debug/SpriteAnimationPanel.hpp"
#include "debug/SpriteSlicerPanel.hpp"
#include "debug/TilePaintingState.hpp"
#include "debug/ParticlePanel.hpp"
#include "debug/PixelArtPanel.hpp"
#include "debug/TilePalettePanel.hpp"
#include "debug/UiCanvasPanel.hpp"
#include "debug/ViewportPanel.hpp"
#include "AetherCore.hpp"
#include "PlaySession.hpp"
#include "assets/AssetManager.hpp"
#include "assets/TileAssetStore.hpp"
#include "io/PlatformPaths.hpp"
#include "mesh/Mesh.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "platform/Window.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/Components.hpp"
#include "PlayState.hpp"
#include "scene/ModelBakeHook.hpp"
#include "editor/EditorProjectContext.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "utils/SettingsService.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/FuzzyMatch.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/LogRingBuffer.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/StringUtils.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	namespace
	{
		// Ask the OS to open a file or folder with whatever is associated with it. Same shape
		// as the one in BuildPanel; kept local rather than shared because two call sites is
		// not yet a reason for a header.
		void OpenPathInShell(const std::filesystem::path& path)
		{
#ifdef _WIN32
			if (path.empty())
			{
				return;
			}
			ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			static_cast<void>(path);
#endif
		}

		// The docs folder as seen from a running editor. It runs out of a build tree during
		// development and out of an install directory otherwise, so walk up from the executable
		// the way the asset mounts do. An empty result greys the menu item out rather than
		// opening nothing - a menu entry that silently does nothing is worse than one that is
		// visibly unavailable.
		std::filesystem::path FindDocsFile(const std::filesystem::path& leaf)
		{
			const std::filesystem::path exeDir = io::PlatformPaths::GetExecutableDir();
			const std::array<std::filesystem::path, 6> roots = {
			        exeDir / "docs",
			        exeDir / ".." / "docs",
			        exeDir / ".." / ".." / "docs",
			        exeDir / ".." / ".." / ".." / "docs",
			        exeDir / ".." / ".." / ".." / ".." / "docs",
			        std::filesystem::current_path() / "docs",
			};
			std::error_code ec;
			for (const std::filesystem::path& root: roots)
			{
				const std::filesystem::path candidate = leaf.empty() ? root : root / leaf;
				if (std::filesystem::exists(candidate, ec))
				{
					return candidate.lexically_normal();
				}
			}
			return {};
		}

		// Built-in dock layouts for common workflows. Each one rebuilds the dockspace
		// via DockBuilder (like Reset Layout) and drives which panels are shown, so a
		// user can jump between "2D authoring", "look-dev", "scripting", etc. in a click.
		enum class WorkflowLayout
		{
			Default = 0,
			TwoD,
			ThreeD,
			Rendering,
			Materials,
			Assets,
			Scripting,
			Minimal,
			Count,
		};

		struct WorkflowLayoutDef
		{
			WorkflowLayout id;
			const char* name;
			const char* tooltip;
		};

		constexpr WorkflowLayoutDef kWorkflowLayouts[] = {
		        {WorkflowLayout::Default, "Default", "General editing: hierarchy, viewport, inspector and all tool panels."},
		        {WorkflowLayout::TwoD, "2D / Sprites", "Tilemaps, sprite slicing, animation and pixel art around the viewport."},
		        {WorkflowLayout::ThreeD, "3D / Scene", "Scene building with lighting; 2D tool panels hidden."},
		        {WorkflowLayout::Rendering, "Rendering / Look-dev", "Render graph, tonemap, post-processing, lighting and textures."},
		        {WorkflowLayout::Materials, "Materials / Shading", "The material graph front and centre, with the file explorer and a viewport to check it against."},
		        {WorkflowLayout::Assets, "Assets / Import", "Browsing and inspecting project files: explorer, textures, materials and the asset inspector."},
		        {WorkflowLayout::Scripting, "Scripting / Debug", "Console, control server, performance and debug tools along the bottom."},
		        {WorkflowLayout::Minimal, "Minimal", "Just hierarchy, viewport and inspector."},
		};

		// Panels shown for a workflow (others are hidden), keyed by panel GetName() - which is
		// also the dock window's title. Three panels used to disagree with their own window
		// (Scene Outliner/Scene, DevTools/Debug, TextureInspector/Textures), so a workflow
		// naming one of them, or anything else looking a panel up by name, silently missed it.
		// Empty => show everything.
		std::vector<std::string> WorkflowVisiblePanels(WorkflowLayout kind)
		{
			switch (kind)
			{
				case WorkflowLayout::TwoD:
					return {"Scene", "Project", "File Explorer", "Viewport", "Inspector", "Tile Palette", "Sprite Slicer", "Sprite Animation", "Pixel Art", "UI Canvas", "Console"};
				case WorkflowLayout::ThreeD:
					return {"Scene", "Project", "File Explorer", "Viewport", "Inspector", "Console", "Performance"};
				case WorkflowLayout::Rendering:
					return {"Scene", "Viewport", "Inspector", "Render Graph", "Post Processing", "Textures", "Performance", "Console"};
				case WorkflowLayout::Materials:
					return {"File Explorer", "Project", "Material", "Viewport", "Inspector", "Scene", "Console"};
				case WorkflowLayout::Assets:
					return {"File Explorer", "Project", "Build", "Material", "Textures", "Inspector", "Viewport", "Console"};
				case WorkflowLayout::Scripting:
					return {"Scene", "Project", "Build", "File Explorer", "Viewport", "Inspector", "Console", "Control Server", "Performance", "Dev Tools"};
				case WorkflowLayout::Minimal:
					return {"Scene", "Viewport", "Inspector"};
				case WorkflowLayout::Default:
				case WorkflowLayout::Count:
					break;
			}
			return {}; // Default: leave everything visible
		}

		// Reset the dockspace and arrange panels for the given workflow.
		// 'allPanels' is every panel the editor owns. Each layout below docks the windows it
		// has an opinion about; a panel added later appears in none of those lists, and an
		// undocked panel becomes its own floating OS window sized to whatever it auto-fits
		// to. So every panel is docked to a sensible fallback FIRST and the explicit calls
		// then override it - DockBuilderDockWindow keeps the last assignment, so named
		// windows still land exactly where each layout intends.
		void BuildWorkflowLayout(WorkflowLayout kind, ImGuiID id, ImVec2 size, std::span<const std::string_view> allPanels)
		{
			ImGui::DockBuilderRemoveNode(id);
			ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(id, size);
			ImGuiID root = id;

			const auto dockRemainingTo = [&](ImGuiID node)
			{
				for (const std::string_view name: allPanels)
				{
					ImGui::DockBuilderDockWindow(std::string(name).c_str(), node);
				}
			};

			switch (kind)
			{
				case WorkflowLayout::Minimal:
				{
					const ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.20f, nullptr, &root);
					const ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.25f, nullptr, &root);
					dockRemainingTo(right);
					ImGui::DockBuilderDockWindow("Scene", left);
					ImGui::DockBuilderDockWindow("Inspector", right);
					ImGui::DockBuilderDockWindow("Build", right);
					ImGui::DockBuilderDockWindow("Viewport", root);
					break;
				}
				case WorkflowLayout::TwoD:
				{
					ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.18f, nullptr, &root);
					const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
					ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.24f, nullptr, &root);
					const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.50f, nullptr, &right);
					const ImGuiID bottom = ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.30f, nullptr, &root);
					dockRemainingTo(rightBottom);
					ImGui::DockBuilderDockWindow("Scene", left);
					ImGui::DockBuilderDockWindow("Project", leftBottom);
					ImGui::DockBuilderDockWindow("Build", leftBottom);
					ImGui::DockBuilderDockWindow("File Explorer", leftBottom);
					ImGui::DockBuilderDockWindow("Inspector", right);
					ImGui::DockBuilderDockWindow("Tile Palette", rightBottom);
					ImGui::DockBuilderDockWindow("Sprite Slicer", bottom);
					ImGui::DockBuilderDockWindow("Sprite Animation", bottom);
					ImGui::DockBuilderDockWindow("Pixel Art", bottom);
					ImGui::DockBuilderDockWindow("Console", bottom);
					ImGui::DockBuilderDockWindow("Viewport", root);
					ImGui::DockBuilderDockWindow("UI Canvas", root);
					break;
				}
				case WorkflowLayout::ThreeD:
				{
					ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.20f, nullptr, &root);
					const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, nullptr, &left);
					ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.24f, nullptr, &root);
					const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
					const ImGuiID bottom = ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.26f, nullptr, &root);
					dockRemainingTo(rightBottom);
					ImGui::DockBuilderDockWindow("Scene", left);
					ImGui::DockBuilderDockWindow("Project", leftBottom);
					ImGui::DockBuilderDockWindow("Build", leftBottom);
					ImGui::DockBuilderDockWindow("File Explorer", leftBottom);
					ImGui::DockBuilderDockWindow("Inspector", right);
					ImGui::DockBuilderDockWindow("Performance", bottom);
					ImGui::DockBuilderDockWindow("Console", bottom);
					ImGui::DockBuilderDockWindow("Viewport", root);
					break;
				}
				case WorkflowLayout::Rendering:
				{
					const ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.18f, nullptr, &root);
					ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.28f, nullptr, &root);
					const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
					const ImGuiID bottom = ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.26f, nullptr, &root);
					dockRemainingTo(rightBottom);
					ImGui::DockBuilderDockWindow("Scene", left);
					ImGui::DockBuilderDockWindow("Render Graph", right);
					ImGui::DockBuilderDockWindow("Post Processing", right);
					ImGui::DockBuilderDockWindow("Inspector", rightBottom);
					ImGui::DockBuilderDockWindow("Build", rightBottom);
					ImGui::DockBuilderDockWindow("Performance", bottom);
					ImGui::DockBuilderDockWindow("Textures", bottom);
					ImGui::DockBuilderDockWindow("Console", bottom);
					ImGui::DockBuilderDockWindow("Viewport", root);
					break;
				}
				case WorkflowLayout::Materials:
				{
					// The node canvas needs the widest node in the layout, so the Material
					// window takes the centre and the viewport moves to the side - the scene is
					// a reference here rather than the thing being edited.
					const ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.17f, nullptr, &root);
					ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.32f, nullptr, &root);
					const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
					const ImGuiID bottom = ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.20f, nullptr, &root);
					dockRemainingTo(rightBottom);
					ImGui::DockBuilderDockWindow("File Explorer", left);
					ImGui::DockBuilderDockWindow("Project", left);
					ImGui::DockBuilderDockWindow("Viewport", right);
					ImGui::DockBuilderDockWindow("Scene", rightBottom);
					ImGui::DockBuilderDockWindow("Inspector", rightBottom);
					ImGui::DockBuilderDockWindow("Console", bottom);
					ImGui::DockBuilderDockWindow("Material", root);
					break;
				}
				case WorkflowLayout::Assets:
				{
					// Browsing, so the explorer gets real width rather than a strip, and the
					// things that inspect one file sit around it.
					ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.26f, nullptr, &root);
					const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, nullptr, &left);
					const ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.30f, nullptr, &root);
					const ImGuiID bottom = ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.30f, nullptr, &root);
					dockRemainingTo(right);
					ImGui::DockBuilderDockWindow("File Explorer", left);
					ImGui::DockBuilderDockWindow("Project", leftBottom);
					ImGui::DockBuilderDockWindow("Build", leftBottom);
					ImGui::DockBuilderDockWindow("Material", right);
					ImGui::DockBuilderDockWindow("Inspector", right);
					ImGui::DockBuilderDockWindow("Textures", bottom);
					ImGui::DockBuilderDockWindow("Console", bottom);
					ImGui::DockBuilderDockWindow("Viewport", root);
					break;
				}
				case WorkflowLayout::Scripting:
				{
					ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.18f, nullptr, &root);
					const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, nullptr, &left);
					const ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.22f, nullptr, &root);
					const ImGuiID bottom = ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.34f, nullptr, &root);
					dockRemainingTo(right);
					ImGui::DockBuilderDockWindow("Scene", left);
					ImGui::DockBuilderDockWindow("Project", leftBottom);
					ImGui::DockBuilderDockWindow("Build", leftBottom);
					ImGui::DockBuilderDockWindow("File Explorer", leftBottom);
					ImGui::DockBuilderDockWindow("Inspector", right);
					ImGui::DockBuilderDockWindow("Console", bottom);
					ImGui::DockBuilderDockWindow("Control Server", bottom);
					ImGui::DockBuilderDockWindow("Performance", bottom);
					ImGui::DockBuilderDockWindow("Dev Tools", bottom);
					ImGui::DockBuilderDockWindow("Viewport", root);
					break;
				}
				case WorkflowLayout::Default:
				case WorkflowLayout::Count:
				{
					ImGuiID left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.20f, nullptr, &root);
					const ImGuiID leftFiles = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, nullptr, &left);
					ImGuiID right = ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.27f, nullptr, &root);
					const ImGuiID rightTools = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.38f, nullptr, &right);
					const ImGuiID bottom = ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.28f, nullptr, &root);
					dockRemainingTo(rightTools);
					ImGui::DockBuilderDockWindow("Scene", left);
					ImGui::DockBuilderDockWindow("Project", leftFiles);
					ImGui::DockBuilderDockWindow("Build", leftFiles);
					ImGui::DockBuilderDockWindow("File Explorer", leftFiles);
					ImGui::DockBuilderDockWindow("Viewport", root);
					ImGui::DockBuilderDockWindow("UI Canvas", root);
					ImGui::DockBuilderDockWindow("Sprite Slicer", root);
					ImGui::DockBuilderDockWindow("Tile Palette", root);
					ImGui::DockBuilderDockWindow("Sprite Animation", root);
					ImGui::DockBuilderDockWindow("Pixel Art", root);
					ImGui::DockBuilderDockWindow("Particles", root);
					ImGui::DockBuilderDockWindow("Inspector", right);
					ImGui::DockBuilderDockWindow("Render Graph", rightTools);
					ImGui::DockBuilderDockWindow("Dev Tools", rightTools);
					ImGui::DockBuilderDockWindow("Post Processing", rightTools);
					ImGui::DockBuilderDockWindow("Settings", rightTools);
					ImGui::DockBuilderDockWindow("Theme", rightTools);
					ImGui::DockBuilderDockWindow("Control Server", rightTools);
					ImGui::DockBuilderDockWindow("Performance", bottom);
					ImGui::DockBuilderDockWindow("Console", bottom);
					ImGui::DockBuilderDockWindow("Textures", bottom);
					break;
				}
			}

			ImGui::DockBuilderFinish(id);
		}

		void AppendObbEdges(std::vector<DebugVertex>& out, const glm::mat4& m, const glm::vec3& mn, const glm::vec3& mx, const glm::vec4& color)
		{
			glm::vec3 corners[8];
			for (int i = 0; i < 8; ++i)
			{
				const glm::vec3 local{((i & 1) != 0) ? mx.x : mn.x, ((i & 2) != 0) ? mx.y : mn.y, ((i & 4) != 0) ? mx.z : mn.z};
				corners[i] = glm::vec3(m * glm::vec4(local, 1.0f));
			}
			static constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
			for (const auto& edge: kEdges)
			{
				AddDebugLine(out, corners[edge[0]], corners[edge[1]], color);
			}
		}

		const char* WindowMenuIcon(std::string_view panelName)
		{
			if (panelName == "Scene")
			{
				return ICON_FA_SITEMAP;
			}
			if (panelName == "Inspector")
			{
				return ICON_FA_MAGNIFYING_GLASS;
			}
			if (panelName == "File Explorer")
			{
				return ICON_FA_FOLDER_OPEN;
			}
			if (panelName == "Project")
			{
				return ICON_FA_CUBE;
			}
			if (panelName == "Viewport")
			{
				return ICON_FA_EYE;
			}
			if (panelName == "UI Canvas")
			{
				return ICON_FA_IMAGE;
			}
			if (panelName == "Render Graph")
			{
				return ICON_FA_DIAGRAM_PROJECT;
			}
			if (panelName == "Post Processing")
			{
				return ICON_FA_WAND_MAGIC_SPARKLES;
			}
			if (panelName == "Textures")
			{
				return ICON_FA_IMAGE;
			}
			if (panelName == "Sprite Slicer")
			{
				return ICON_FA_IMAGE;
			}
			if (panelName == "Sprite Animation")
			{
				return ICON_FA_FILM;
			}
			if (panelName == "Console")
			{
				return ICON_FA_CODE;
			}
			if (panelName == "Performance")
			{
				return ICON_FA_GAUGE_HIGH;
			}
			if (panelName == "Dev Tools")
			{
				return ICON_FA_BUG;
			}
			if (panelName == "Settings")
			{
				return ICON_FA_GEARS;
			}
			if (panelName == "Theme")
			{
				return ICON_FA_PALETTE;
			}
			return ICON_FA_CIRCLE;
		}

		std::string PanelVisibilityKey(std::string_view panelName)
		{
			std::string key = "debug.window.";
			for (const char c: panelName)
			{
				key += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
			}
			return key;
		}

		std::filesystem::path EditorStatePath()
		{
			return io::PlatformPaths::GetUserConfigDir() / "EditorState.toml";
		}

	} // namespace

	void DebugLayer::LoadSettings(app::LayerContext&)
	{
		const auto path = EditorStatePath();
		if (!path.empty() && m_debugConfig.LoadFromPath(path))
		{
			AE_INFO(LogCategory::App, "Editor state loaded from {}", path.string());
		}
		m_projects.LoadSettings(m_debugConfig);
	}

	void DebugLayer::SaveSettings(app::LayerContext&)
	{
		if (!m_debugConfig.IsDirty())
		{
			return;
		}
		const auto path = EditorStatePath();
		if (!path.empty() && m_debugConfig.SaveToPath(path, "AetherCore editor state"))
		{
			m_debugConfig.MarkClean();
			AE_INFO(LogCategory::App, "Editor state saved to {}", path.string());
		}
	}

	void DebugLayer::PersistSettings(app::LayerContext& context)
	{
		for (auto& panel: m_panels)
		{
			panel->SaveSettings(m_debugConfig, context);
			m_debugConfig.Set(PanelVisibilityKey(panel->GetName()), panel->IsVisible());
		}
		m_projects.SaveSettings(m_debugConfig);

		// Persist the editor window size (guarded so an unchanged size never dirties
		if (m_editorWindowW > 0 && static_cast<int>(m_debugConfig.GetFloat("editor.window_width", -1.0f)) != m_editorWindowW)
		{
			m_debugConfig.Set("editor.window_width", static_cast<float>(m_editorWindowW));
		}
		if (m_editorWindowH > 0 && static_cast<int>(m_debugConfig.GetFloat("editor.window_height", -1.0f)) != m_editorWindowH)
		{
			m_debugConfig.Set("editor.window_height", static_cast<float>(m_editorWindowH));
		}

		SaveSettings(context);
	}

	void DebugLayer::OnAttach(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();
		m_projects.Attach(context.services);
		LoadSettings(context);

		int defW = 2560;
		int defH = 1440;
		if (auto* settings = context.services.TryGet<SettingsService>())
		{
			defW = settings->Get().window.width;
			defH = settings->Get().window.height;
		}
		m_editorWindowW = static_cast<int>(m_debugConfig.GetFloat("editor.window_width", static_cast<float>(defW)));
		m_editorWindowH = static_cast<int>(m_debugConfig.GetFloat("editor.window_height", static_cast<float>(defH)));

		// panel can resolve it for its whole lifetime.
		context.services.Register<SceneSelection>(m_selection);
		context.services.Register<UndoStack>(m_undoStack);
		context.services.Register<TilePaintingState>(m_tilePainting);
		context.services.Register<PixelArtDocument>(m_pixelArt);
		m_panels.push_back(std::make_unique<BuildPanel>());
		m_panels.push_back(std::make_unique<RenderGraphPanel>());
		m_panels.push_back(std::make_unique<TextureInspectorPanel>());
		m_panels.push_back(std::make_unique<SpriteSlicerPanel>());
		m_panels.push_back(std::make_unique<TilePalettePanel>());
		m_panels.push_back(std::make_unique<PixelArtPanel>());
		m_panels.push_back(std::make_unique<SpriteAnimationPanel>());
		m_panels.push_back(std::make_unique<ParticlePanel>());
		auto hierarchyPanel = std::make_unique<HierarchyPanel>();
		m_hierarchyPanel = hierarchyPanel.get();
		m_panels.push_back(std::move(hierarchyPanel));
		m_panels.push_back(std::make_unique<ProjectPanel>());
		m_panels.push_back(std::make_unique<FileExplorerPanel>());
		m_panels.push_back(std::make_unique<InspectorPanel>());
		m_panels.push_back(std::make_unique<MaterialGraphPanel>());
		m_panels.push_back(std::make_unique<UiCanvasPanel>());
		m_panels.push_back(std::make_unique<PerformancePanel>());
		{
			auto viewportPanel = std::make_unique<ViewportPanel>();
			m_viewportPanel = viewportPanel.get();
			m_panels.push_back(std::move(viewportPanel));
		}
		m_panels.push_back(std::make_unique<PostProcessingPanel>());
		m_panels.push_back(std::make_unique<SettingsPanel>());
		m_panels.push_back(std::make_unique<ThemePanel>());
		m_panels.push_back(std::make_unique<DevToolsPanel>());
		m_panels.push_back(std::make_unique<ConsolePanel>());
		m_panels.push_back(std::make_unique<ControlServerPanel>());
		for (auto& panel: m_panels)
		{
			panel->OnAttach(context);
		}

		// Load persisted named layout presets (Window > Layouts / command palette).
		ReloadLayoutPresets();
		for (auto& panel: m_panels)
		{
			panel->LoadSettings(m_debugConfig, context);
			panel->SetVisible(m_debugConfig.GetBool(PanelVisibilityKey(panel->GetName()), panel->DefaultVisible()));
		}

		// main thread (same as OnImGui), so mutating visibility here is race-free.
		EditorWindowActions windowActions;
		windowActions.list = [this]()
		{
			std::vector<EditorWindowInfo> out;
			out.reserve(m_panels.size());
			for (const auto& panel: m_panels)
			{
				out.push_back({std::string(panel->GetName()), panel->IsVisible()});
			}
			return out;
		};
		windowActions.setVisible = [this](std::string_view name, bool visible) -> bool
		{
			const auto equalsIgnoreCase = [](std::string_view a, std::string_view b)
			{
				if (a.size() != b.size())
				{
					return false;
				}
				for (std::size_t i = 0; i < a.size(); ++i)
				{
					if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
					{
						return false;
					}
				}
				return true;
			};
			for (auto& panel: m_panels)
			{
				if (equalsIgnoreCase(panel->GetName(), name))
				{
					panel->SetVisible(visible);
					return true;
				}
			}
			return false;
		};
		windowActions.focusWindow = [this](std::string_view name) -> bool
		{
			for (auto& panel: m_panels)
			{
				const std::string_view panelName = panel->GetName();
				if (panelName.size() != name.size())
				{
					continue;
				}
				bool same = true;
				for (std::size_t i = 0; i < panelName.size(); ++i)
				{
					if (std::tolower(static_cast<unsigned char>(panelName[i])) != std::tolower(static_cast<unsigned char>(name[i])))
					{
						same = false;
						break;
					}
				}
				if (same)
				{
					// Focusing a hidden panel would put a tab nobody can see in front, so it
					// is shown first - which is what someone asking to focus it meant anyway.
					panel->SetVisible(true);
					m_pendingFocusWindow = panelName;
					return true;
				}
			}
			return false;
		};
		windowActions.focusInspectorComponent = [this](std::string_view component)
		{
			if (DebugPanel* inspector = FindPanelByName("Inspector"))
			{
				inspector->SetVisible(true);
			}
			iw::InspectorFocusRequest() = std::string(component);
		};
		windowActions.historyStep = [this](bool redo)
		{
			if (redo)
			{
				++m_pendingRedoSteps;
			}
			else
			{
				++m_pendingUndoSteps;
			}
		};
		windowActions.openSceneDialog = [this]()
		{
			// Deferred rather than opening the dialog here: picking a scene in it replaces
			// the world, so it has to pass the same unsaved-work check the File menu does,
			// and that needs the frame's context.
			m_pendingOpenSceneDialog = true;
		};
		windowActions.listLayouts = []()
		{
			std::vector<std::string> names;
			for (const WorkflowLayoutDef& wf: kWorkflowLayouts)
			{
				names.emplace_back(wf.name);
			}
			return names;
		};
		windowActions.applyLayout = [this](std::string_view name) -> bool
		{
			const auto equalsInsensitive = [](std::string_view a, std::string_view b)
			{
				return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
			};
			for (const WorkflowLayoutDef& wf: kWorkflowLayouts)
			{
				if (equalsInsensitive(name, wf.name))
				{
					ApplyWorkflowLayout(static_cast<int>(wf.id));
					return true;
				}
			}
			return false;
		};
		context.services.Register<EditorWindowActions>(m_windowActions = std::move(windowActions));
	}

	void DebugLayer::OnDetach(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		if (m_viewportPanel != nullptr)
		{
			m_viewportPanel->PersistCamera(m_debugConfig, context);
		}
		PersistSettings(context);
		for (auto& panel: m_panels)
		{
			panel->OnDetach(context);
		}
		context.services.Unregister<EditorWindowActions>();
		m_windowActions = {};
		m_panels.clear();
		m_hierarchyPanel = nullptr;
		context.services.Unregister<app::EditorProjectContext>();
		context.services.Unregister<app::scene::ModelBakeHook>();
		context.services.Unregister<EditorProjectActions>();
		context.services.Unregister<UndoStack>();
		context.services.Unregister<SceneSelection>();
		m_projects.Detach();

		m_scriptErrors.Clear();
		m_dockspaceBuilt = false;
	}

	void DebugLayer::DrainSceneWrites(app::LayerContext& context)
	{
		for (const auto& done: m_sceneWriter.TakeCompletions())
		{
			if (done.ok)
			{
				// Only now does the file hold what the recovery copy held, so only now is
				// the copy safe to drop.
				if (const auto* project = context.TryGet<app::EditorProjectContext>())
				{
					editor::AutosaveService::Discard(*project, done.sceneName);
				}
				continue;
			}

			// The save was reported clean when it was handed over, so take that back: the
			// scene is dirty, the recovery copy stays, and the failure is on screen rather
			// than only in the log.
			if (auto* undo = context.TryGet<editor::UndoStack>())
			{
				undo->MarkUnsaved();
			}
			ShowToast(std::string(ICON_FA_TRIANGLE_EXCLAMATION "  Could not write scene '") + done.sceneName + "' - your changes are still unsaved.", true);
		}
	}

	void DebugLayer::OnUpdate(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();
		DrainSceneWrites(context);
		const Input& input = context.Get<Input>();

		if (auto* engine = context.TryGet<AetherCore>())
		{
			// Only the editor is allowed to idle; a game must keep running whether or not
			// anyone is touching the keyboard.
			engine->SetIdleThrottleAllowed(true);

			// ...and not while it is hosting one. A scene in play mode animates, simulates and
			// plays audio with no input at all, so "nothing was pressed" is not idle.
			// Compiling counts too: the reload finishes on a frame, and a throttled editor
			// would sit on the result.
			const auto* playState = context.TryGet<app::PlayState>();
			if (playState != nullptr && (playState->IsPlaying() || playState->IsCompiling()))
			{
				engine->RequestActivity();
			}
		}

		// Editor scene visibility applies while authoring and not while playing, the same way
		// the eye icon behaves in every editor that has one. Driven every frame rather than on
		// the play transition so it cannot be left stuck on by a path that forgot to clear it.
		{
			const auto* visibilityPlayState = context.TryGet<app::PlayState>();
			ecs::EditorSceneVisibilityRespected() = visibilityPlayState == nullptr || !visibilityPlayState->IsPlaying();
		}

		if (input.IsKeyPressed(aether::Key::F5))
		{
			if (auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>())
			{
				scripting->RequestReload();
			}
		}

		m_scriptErrors.Poll(context);

		m_autosave.Tick(context);

		m_projects.UpdateScriptBuild();

		if (!m_projects.IsProjectLoaded())
		{
			return;
		}

		m_selection.Prune(context.Get<World>());

		const auto* updatePlayState = context.TryGet<app::PlayState>();
		const bool editing = updatePlayState == nullptr || !updatePlayState->IsPlaying();
		if (editing && IsDebugRenderingEnabled() && !m_selection.All().empty())
		{
			if (auto* engine = context.TryGet<AetherCore>())
			{
				if (m_selection.ChangeSerial() != m_outlineSeenSerial)
				{
					m_outlineSeenSerial = m_selection.ChangeSerial();
					m_outlinePulseStart = context.elapsedTimeSeconds;
				}
				const float pulseT = m_outlinePulseStart >= 0.0 ? std::clamp(static_cast<float>((context.elapsedTimeSeconds - m_outlinePulseStart) / 0.5), 0.0f, 1.0f) : 1.0f;
				const float brightness = 1.6f - 0.6f * pulseT;

				auto& verts = engine->GetPendingDebugVertices();
				World& world = context.Get<World>();
				const Entity primary = m_selection.Primary();
				for (const Entity e: m_selection.All())
				{
					if (world.Has<SpriteRendererComponent>(e))
					{
						continue;
					}
					const auto* tc = world.TryGet<TransformComponent>(e);
					if (tc == nullptr)
					{
						continue;
					}
					glm::vec3 mn{-0.125f};
					glm::vec3 mx{0.125f};
					if (const auto* mc = world.TryGet<MeshComponent>(e); mc != nullptr && mc->mesh != nullptr && mc->mesh->GetAABBMin() != mc->mesh->GetAABBMax())
					{
						mn = mc->mesh->GetAABBMin();
						mx = mc->mesh->GetAABBMax();
					}
					const float alpha = (e == primary) ? 1.0f : 0.45f;
					const glm::vec4 gold{1.0f * brightness, 0.72f * brightness, 0.2f * brightness, alpha};
					AppendObbEdges(verts, tc->localToWorld, mn, mx, gold);
				}
			}
		}

		for (auto& panel: m_panels)
		{
			panel->OnUpdate(context);
		}
	}

	void DebugLayer::OnRenderTargetsInvalidated(app::LayerContext& context)
	{
		for (auto& panel: m_panels)
		{
			panel->OnRenderTargetsInvalidated(context);
		}
	}

	void DebugLayer::DrawStatusBar(app::LayerContext& context)
	{
		using namespace chrome;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
		if (ImGui::BeginChild("##StatusBar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar))
		{
			const auto* playState = context.TryGet<app::PlayState>();
			const bool playing = playState != nullptr && playState->IsPlaying();
			const bool compiling = playState != nullptr && playState->IsCompiling();

			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 barMin = ImGui::GetWindowPos();
			const float barW = ImGui::GetWindowWidth();

			if (playing || compiling)
			{
				AccentHairline(drawList, barMin, barW, playing ? 0.85f : 0.35f);
			}

			ImGui::AlignTextToFramePadding();

			const auto tick = [&](const ImVec4& color)
			{
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float h = ImGui::GetTextLineHeight();
				drawList->AddRectFilled(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + 3.0f, p.y + h - 1.0f), U32(color));
				ImGui::Dummy(ImVec2(9.0f, 0.0f));
				ImGui::SameLine();
			};
			const auto divider = [&]()
			{
				ImGui::SameLine(0.0f, 12.0f);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float h = ImGui::GetTextLineHeight();
				drawList->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x, p.y + h - 1.0f), U32(WithAlpha(kStroke, 0.9f)), 1.0f);
				ImGui::Dummy(ImVec2(0.0f, 0.0f));
				ImGui::SameLine(0.0f, 12.0f);
			};

			ImGui::Dummy(ImVec2(2.0f, 0.0f));
			ImGui::SameLine();

			if (m_projects.HasCurrentProject())
			{
				tick(kAccent);
				ImGui::PushStyleColor(ImGuiCol_Text, kText);
				ImGui::Text(ICON_FA_FOLDER_OPEN "  %s", m_projects.CurrentProject().name.c_str());
				ImGui::PopStyleColor();
				divider();
			}

			const char* sceneName = "-";
			if (const auto* scenes = context.TryGet<SceneSubsystem>(); scenes != nullptr && !scenes->GetCurrentScene().empty())
			{
				sceneName = scenes->GetCurrentScene().c_str();
			}
			// The unsaved dot is the only thing on screen that answers "have I saved?".
			// Tracking for it already existed and was read by nothing but the autosave.
			const bool unsaved = HasUnsavedWork();
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::Text(ICON_FA_CUBE "  %s%s", sceneName, unsaved ? " *" : "");
			ImGui::PopStyleColor();
			if (unsaved && ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Unsaved changes  -  Ctrl+S to save");
			}

			// SCRIPT errors specifically, not every log line. A count of everything the engine
			// has ever warned about is noise you learn to ignore; a broken script is the thing
			// you are about to go and fix, and it is the reason play mode will not behave.
			if (const auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>(); scripting != nullptr && !scripting->ScriptErrors().empty())
			{
				divider();
				const std::vector<std::string>& scriptErrors = scripting->ScriptErrors();
				char logLabel[64];
				std::snprintf(logLabel, sizeof(logLabel), ICON_FA_CIRCLE_EXCLAMATION "  %zu##scriptErrors", scriptErrors.size());
				ImGui::PushStyleColor(ImGuiCol_Text, C(colors::Error));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, WithAlpha(C(colors::Error), 0.18f));
				// Measured with the ID suffix HIDDEN. CalcTextSize keeps everything after "##"
				// by default, so the clickable band stretched far past the badge - a wide strip
				// of the status bar lit up and answered a click meant for the icon.
				const float logWidth = ImGui::CalcTextSize(logLabel, nullptr, true).x;
				if (ImGui::Selectable(logLabel, false, ImGuiSelectableFlags_None, ImVec2(logWidth, 0.0f)))
				{
					// Show the errors themselves. Focusing the Console alone was not enough: it
					// follows its newest lines, so the failure sat above the view.
					m_pendingFocusWindow = "Console";
					if (auto* console = dynamic_cast<ConsolePanel*>(FindPanelByName("Console")))
					{
						console->SetVisible(true);
						console->ShowScriptErrors();
					}
				}
				ImGui::PopStyleColor(2);
				if (ImGui::IsItemHovered())
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					ImGui::SetTooltip("%zu script error(s)  -  click to see them", scriptErrors.size());
				}
			}

			// Engine problems, counted apart from the script ones. A shader that fails to
			// compile on open, an asset that will not load - none of it is a script error, and
			// without this the status bar says the project is healthy while it is not. Kept
			// visually quieter than the script badge, which is the one you usually act on.
			{
				const LogRingBuffer::LevelCounts logCounts = LogRingBuffer::Get().Counts();
				std::size_t scriptErrorCount = 0;
				if (const auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>(); scripting != nullptr)
				{
					scriptErrorCount = scripting->ScriptErrors().size();
				}
				// Every script error is also logged once, so subtracting leaves what the engine
				// itself reported and stops the two badges counting the same failure twice.
				const std::size_t engineErrors = logCounts.error > scriptErrorCount ? logCounts.error - scriptErrorCount : 0u;
				if (engineErrors > 0 || logCounts.warn > 0)
				{
					divider();
					const bool hasErrors = engineErrors > 0;
					char engineLabel[64];
					std::snprintf(engineLabel, sizeof(engineLabel), "%s  %zu##engineLog", hasErrors ? ICON_FA_CIRCLE_EXCLAMATION : ICON_FA_TRIANGLE_EXCLAMATION, hasErrors ? engineErrors : logCounts.warn);
					ImGui::PushStyleColor(ImGuiCol_Text, hasErrors ? C(colors::Error) : C(colors::Orange));
					ImGui::PushStyleColor(ImGuiCol_HeaderHovered, WithAlpha(hasErrors ? C(colors::Error) : C(colors::Orange), 0.18f));
					const float engineWidth = ImGui::CalcTextSize(engineLabel, nullptr, true).x;
					if (ImGui::Selectable(engineLabel, false, ImGuiSelectableFlags_None, ImVec2(engineWidth, 0.0f)))
					{
						m_pendingFocusWindow = "Console";
						if (auto* console = dynamic_cast<ConsolePanel*>(FindPanelByName("Console")))
						{
							console->SetVisible(true);
							console->ShowLatestProblem();
						}
					}
					ImGui::PopStyleColor(2);
					if (ImGui::IsItemHovered())
					{
						ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
						ImGui::SetTooltip("%zu engine error(s), %zu warning(s)  -  click to see them", engineErrors, logCounts.warn);
					}
				}
			}

			{
				const char* label = playing ? ICON_FA_PLAY "  PLAYING" : (compiling ? ICON_FA_GEAR "  COMPILING" : ICON_FA_STOP "  EDITING");
				const ImVec2 textSize = ImGui::CalcTextSize(label);
				const ImVec4 color = (playing || compiling) ? kAccentHi : kMuted;
				const ImVec2 textPos(barMin.x + (barW - textSize.x) * 0.5f, barMin.y + (ImGui::GetWindowHeight() - textSize.y) * 0.5f);
				drawList->AddText(textPos, U32(color), label);
			}

			// scripts on a worker thread (project open / F5), show what's happening with
			if (const auto* buildScripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>(); buildScripting != nullptr && buildScripting->IsBuilding() && !compiling)
			{
				divider();
				tick(kAccent);
				ImGui::PushStyleColor(ImGuiCol_Text, kAccentHi);
				ImGui::TextUnformatted(ICON_FA_GEAR "  Compiling C# scripts");
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 10.0f);
				const float trackW = 120.0f;
				const float trackH = 3.0f;
				const float segW = trackW * 0.34f;
				const ImVec2 curPos = ImGui::GetCursorScreenPos();
				const float trackY = curPos.y + (ImGui::GetFrameHeight() - trackH) * 0.5f;
				const float radius = trackH * 0.5f;
				drawList->AddRectFilled(ImVec2(curPos.x, trackY), ImVec2(curPos.x + trackW, trackY + trackH), U32(WithAlpha(kAccent, 0.20f)), radius);
				const double sweep = ImGui::GetTime() * 0.8;
				const float u = static_cast<float>(sweep - std::floor(sweep));
				const float segX = curPos.x + u * (trackW - segW);
				drawList->AddRectFilled(ImVec2(segX, trackY), ImVec2(segX + segW, trackY + trackH), U32(kAccentHi), radius);
				ImGui::Dummy(ImVec2(trackW, 0.0f));
			}

			const ImGuiIO& io = ImGui::GetIO();
			gpu::Extent2D extent{};
			if (const auto* swapchain = context.TryGet<Swapchain>())
			{
				extent = swapchain->GetExtent();
			}
			const float frameMs = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
			const std::string res = std::format("{}x{}", extent.width, extent.height);
			// While the idle throttle is engaged the editor is deliberately drawing at a few
			// frames a second, and the raw numbers ("10 FPS", "100.00 MS") read as a broken
			// editor rather than a resting one. This is the always-visible readout, so it says
			// what is actually going on instead.
			const auto* idleEngine = context.TryGet<AetherCore>();
			const bool idleThrottled = idleEngine != nullptr && idleEngine->IsIdleThrottled();
			const std::string fps = idleThrottled ? std::string("IDLE") : std::format("{:.0f} FPS", io.Framerate);
			const std::string ms = idleThrottled ? std::string() : std::format("{:.2f} MS", frameMs);
			const float gap = 18.0f;
			const float totalW = ImGui::CalcTextSize(ICON_FA_GAUGE_HIGH).x + 8.0f + ImGui::CalcTextSize(res.c_str()).x + gap + ImGui::CalcTextSize(fps.c_str()).x + gap + ImGui::CalcTextSize(ms.c_str()).x + 12.0f;
			const float targetX = ImGui::GetWindowWidth() - totalW;
			if (targetX > ImGui::GetCursorPosX())
			{
				ImGui::SameLine(targetX);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, kAccentHi);
			ImGui::TextUnformatted(ICON_FA_GAUGE_HIGH);
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, 8.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, kFaint);
			ImGui::TextUnformatted(res.c_str());
			ImGui::SameLine(0.0f, gap);
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_Text, idleThrottled ? kFaint : kText);
			ImGui::TextUnformatted(fps.c_str());
			ImGui::PopStyleColor();
			if (idleThrottled)
			{
				ImGui::SetItemTooltip("Nothing is happening, so the editor has dropped to %.0f fps to save power. It returns to full speed the moment you interact with it.", static_cast<double>(io.Framerate));
			}
			if (!ms.empty())
			{
				ImGui::SameLine(0.0f, gap);
				ImGui::PushStyleColor(ImGuiCol_Text, kFaint);
				ImGui::TextUnformatted(ms.c_str());
				ImGui::PopStyleColor();
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	void DebugLayer::DrawShortcutsReference()
	{
		// Rendered from shortcuts::All(), which is the same table the call sites index into,
		// so this cannot describe a key the editor does not actually use.
		struct Group
		{
			const char* title;
			shortcuts::Context context;
		};

		static constexpr Group kGroups[] = {
		        {"Global", shortcuts::Context::Global},
		        {"Scene  (Hierarchy)", shortcuts::Context::Scene},
		        {"Viewport", shortcuts::Context::Viewport},
		        {"Tile Palette", shortcuts::Context::TilePalette},
		        {"Assets", shortcuts::Context::Assets},
		        {"UI Canvas", shortcuts::Context::UiCanvas},
		        {"Material Graph", shortcuts::Context::MaterialGraph},
		};

		constexpr const char* kTitle = "Keyboard Shortcuts###shortcuts";
		if (m_openShortcuts)
		{
			ImGui::OpenPopup(kTitle);
			m_openShortcuts = false;
		}
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}
		// Two columns, because the list grows every time a panel gains a binding and an
		// auto-resizing popup has no scrollbar: one column would eventually run off the
		// bottom of a shorter screen with no way to reach the rest.
		//
		// Laid out with groups and measured text rather than tables. A table asks for the
		// width available to it, an auto-resizing popup derives its width from its content,
		// and nesting the two made that circular: the popup settled 200px narrower than its
		// content and quietly cut every description ("Command palette" rendered as "Co").
		const float accelWidth = [] {
			float widest = 0.0f;
			for (const shortcuts::Binding& entry: shortcuts::All())
			{
				widest = std::max(widest, ImGui::CalcTextSize(entry.display).x);
			}
			return widest + ImGui::GetStyle().ItemSpacing.x * 2.0f;
		}();

		const int half = (static_cast<int>(shortcuts::All().size()) + 1) / 2;
		int drawn = 0;
		bool wrapped = false;
		ImGui::BeginGroup();
		for (const Group& group: kGroups)
		{
			// Break to the second column between sections, never inside one.
			if (!wrapped && drawn >= half)
			{
				ImGui::EndGroup();
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 3.0f);
				ImGui::BeginGroup();
				wrapped = true;
			}
			ImGui::SeparatorText(group.title);
			for (const shortcuts::Binding& entry: shortcuts::All())
			{
				if (entry.context != group.context)
				{
					continue;
				}
				const float rowStart = ImGui::GetCursorPosX();
				ImGui::TextColored(chrome::kAccentHi, "%s", entry.display);
				// SetCursorPosX rather than SameLine(x): both are window-relative, but
				// SameLine's offset is resolved against the line start, which is not the group
				// start - so the second column's descriptions drifted far to the right.
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::SetCursorPosX(rowStart + accelWidth);
				ImGui::TextUnformatted(entry.description);
				++drawn;
			}
		}
		ImGui::EndGroup();
		ImGui::Spacing();
		// Deliberately NOT F1: the key that opened this is still down on the frame the popup
		// first draws, so closing on it too made the window open and shut in the same frame
		// and F1 appeared to do nothing at all.
		if (chrome::PrimaryButton("Close", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void DebugLayer::DrawCommandPalette(app::LayerContext& context)
	{
		const ImGuiIO& io = ImGui::GetIO();
		if ((io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(shortcuts::kCommandPalette.key, false)) || m_openCommandPalette)
		{
			m_openCommandPalette = false;
			m_paletteQuery[0] = '\0';
			m_paletteSelected = 0;
			ImGui::OpenPopup("##CommandPalette");
		}

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.16f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, vp->WorkSize.x - 40.0f), 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopup("##CommandPalette", ImGuiWindowFlags_NoMove))
		{
			return;
		}

		struct Action
		{
			std::string label;
			std::function<void()> run;
		};

		std::vector<Action> actions;
		for (auto& panel: m_panels)
		{
			DebugPanel* p = panel.get();
			actions.push_back({std::string("View: ") + std::string(p->GetName()), [p]() { *p->VisiblePtr() = !*p->VisiblePtr(); }});
		}
		if (auto* playState = context.TryGet<app::PlayState>())
		{
			actions.push_back({"Play: Toggle Play / Stop", [&context]() { TogglePlaySession(context); }});
			actions.push_back({"Play: Pause / Resume (F6)", [&context]() { TogglePausePlaySession(context); }});
			actions.push_back({"Play: Step One Frame (F7)", [&context]() { StepPlaySession(context); }});
			actions.push_back({"Play: Speed 0.5x (slow-mo)", [playState]() { playState->SetTimeScale(0.5f); }});
			actions.push_back({"Play: Speed 1x (normal)", [playState]() { playState->SetTimeScale(1.0f); }});
			actions.push_back({"Play: Speed 2x (fast-forward)", [playState]() { playState->SetTimeScale(2.0f); }});
		}
		actions.push_back({"Help: Keyboard Shortcuts (F1)", [this]() { m_openShortcuts = true; }});
		actions.push_back({"Layout: Reset to Default", [this]() { m_resetLayout = true; }});
		for (const WorkflowLayoutDef& wf: kWorkflowLayouts)
		{
			const int id = static_cast<int>(wf.id);
			actions.push_back({std::string("Layout: ") + wf.name, [this, id]() { ApplyWorkflowLayout(id); }});
		}
		for (const auto& preset: m_layoutPresets)
		{
			actions.push_back({std::string("Layout: ") + preset.name, [this, preset]() { ApplyLayoutPreset(preset); }});
		}

		if (ImGui::IsWindowAppearing())
		{
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		const bool submitted = ImGui::InputTextWithHint("##palettequery", "Type a command...", m_paletteQuery, sizeof(m_paletteQuery), ImGuiInputTextFlags_EnterReturnsTrue);

		struct Ranked
		{
			int score;
			std::size_t index;
		};

		std::vector<Ranked> ranked;
		for (std::size_t i = 0; i < actions.size(); ++i)
		{
			if (const auto s = FuzzyMatch(m_paletteQuery, actions[i].label))
			{
				ranked.push_back({*s, i});
			}
		}
		std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });

		const int count = static_cast<int>(ranked.size());
		if (count > 0)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
			{
				m_paletteSelected = (m_paletteSelected + 1) % count;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
			{
				m_paletteSelected = (m_paletteSelected + count - 1) % count;
			}
			m_paletteSelected = std::clamp(m_paletteSelected, 0, count - 1);
		}
		else
		{
			m_paletteSelected = 0;
		}

		ImGui::Separator();
		ImVec4 paletteSel = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
		paletteSel.w = 1.0f;
		ImGui::PushStyleColor(ImGuiCol_Header, paletteSel);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, paletteSel);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, paletteSel);
		int runIndex = -1;
		for (int i = 0; i < count && i < 12; ++i)
		{
			const Action& action = actions[ranked[static_cast<std::size_t>(i)].index];
			if (ImGui::Selectable(action.label.c_str(), i == m_paletteSelected))
			{
				runIndex = static_cast<int>(ranked[static_cast<std::size_t>(i)].index);
			}
		}
		ImGui::PopStyleColor(3);
		if (submitted && count > 0)
		{
			runIndex = static_cast<int>(ranked[static_cast<std::size_t>(m_paletteSelected)].index);
		}

		if (runIndex >= 0)
		{
			actions[static_cast<std::size_t>(runIndex)].run();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void DebugLayer::ReloadLayoutPresets()
	{
		m_layoutPresets = LayoutPresetStore::LoadAll();
	}

	void DebugLayer::ApplyLayoutPreset(const LayoutPreset& preset)
	{
		m_pendingLayoutIni = preset.imguiIni;
		m_pendingLayoutVisibility = preset.visibility;
		m_pendingLayoutApply = true;
	}

	void DebugLayer::ApplyWorkflowLayout(int index)
	{
		if (index < 0 || index >= static_cast<int>(WorkflowLayout::Count))
		{
			return;
		}
		// Rebuild the dockspace for this workflow next frame...
		m_pendingWorkflowLayout = index;
		m_resetLayout = true;
		// ...and show only the panels that workflow uses (empty set => leave all shown).
		const std::vector<std::string> visible = WorkflowVisiblePanels(static_cast<WorkflowLayout>(index));
		if (!visible.empty())
		{
			for (auto& panel: m_panels)
			{
				const std::string name(panel->GetName());
				panel->SetVisible(std::find(visible.begin(), visible.end(), name) != visible.end());
			}
		}
		else
		{
			for (auto& panel: m_panels)
			{
				panel->SetVisible(true);
			}
		}
	}

	void DebugLayer::CaptureCurrentLayout(std::string name)
	{
		LayoutPreset preset;
		preset.name = std::move(name);
		std::size_t iniSize = 0;
		if (const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize))
		{
			preset.imguiIni.assign(ini, iniSize);
		}
		preset.visibility.reserve(m_panels.size());
		for (const auto& panel: m_panels)
		{
			preset.visibility.emplace_back(std::string(panel->GetName()), panel->IsVisible());
		}
		if (LayoutPresetStore::Save(preset))
		{
			ReloadLayoutPresets();
		}
	}

	void DebugLayer::DeleteLayoutPreset(std::string_view name)
	{
		LayoutPresetStore::Remove(name);
		ReloadLayoutPresets();
	}

	DebugPanel* DebugLayer::FindPanelByName(std::string_view name) const
	{
		for (const auto& panel: m_panels)
		{
			if (panel->GetName() == name)
			{
				return panel.get();
			}
		}
		return nullptr;
	}

	bool DebugLayer::SaveCurrentScene(app::LayerContext& context)
	{
		// Same gate the undo shortcuts use (undoEditable): while Playing/Compiling the
		// live World is the running simulation, not the authored document. Saving here
		// would bake physics-displaced transforms and script-spawned entities into the
		// scene file, MarkSaved() would pin the unsaved-changes baseline to that
		// capture, and FlushDirtyTileMaps() would persist tile paints Stop is about to
		// revert (the play snapshot restores those in memory only). Applies to every
		// caller: Ctrl+S, the File menu, the unsaved-changes popup, and the launcher
		// return. Focused-document saves (material graph, pixel art) are unaffected -
		// those are separate documents, not the scene.
		if (const auto* playState = context.TryGet<app::PlayState>(); playState != nullptr && (playState->IsPlaying() || playState->IsCompiling()))
		{
			ShowToast(std::string(ICON_FA_TRIANGLE_EXCLAMATION "  Cannot save the scene during Play - stop the session first."), true);
			return false;
		}
		auto* scenes = context.TryGet<SceneSubsystem>();
		const std::string currentName = scenes != nullptr ? scenes->GetCurrentScene() : std::string{};

		bool saved = false;
		std::string tileFlushError;
		if (!currentName.empty())
		{
			if (auto* assets = context.TryGet<AssetManager>())
			{
				// Capture from the ECS on the main thread (~2 ms), then hand the heavy
				// serialization + disk write (~15-20 ms) to the background writer so the
				// frame never stalls on a save. Pending writes flush on shutdown, so a
				// last-moment Ctrl+S is never lost.
				app::scene::SceneDescription desc = app::scene::CaptureScene(context.Get<World>(), assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());
				m_sceneWriter.RequestSave(currentName, std::move(desc));

				// Tilemap cells live in their own .tiles asset, not the scene TOML, so a
				// scene save must also flush any edited-in-memory tilemaps to disk - else
				// Play looks right (in-memory) but the saved/published project keeps the
				// stale .tiles.
				if (auto* tiles = context.TryGet<TileAssetStore>())
				{
					if (const auto flushed = tiles->FlushDirtyTileMaps(); flushed.has_value())
					{
						if (auto* paint = context.TryGet<editor::TilePaintingState>())
						{
							paint->mapDirty = false;
						}
					}
					else
					{
						// Do not clear mapDirty (edits are still unsaved) and do not claim a
						// clean save - the .tiles on disk is now out of sync with the editor.
						AE_WARN(LogCategory::App, "Failed to save edited tilemap(s): {}", flushed.error().message);
						tileFlushError = flushed.error().message;
					}
				}
				// The scene's entity edits are now on disk; pin this history position so
				// the unsaved-changes guard only fires on edits made after this save.
				if (auto* undo = context.TryGet<editor::UndoStack>())
				{
					undo->MarkSaved();
				}
				// The recovery copy is NOT dropped here. The write is still in flight on the
				// writer thread, and discarding the copy before it lands means a failed save
				// destroys the one thing that could have recovered the work. DrainSceneWrites
				// discards it once the file is actually on disk.
				saved = true;
			}
		}

		if (saved && !tileFlushError.empty())
		{
			ShowToast(std::string(ICON_FA_TRIANGLE_EXCLAMATION "  Scene saved, but a tilemap could not be written: ") + tileFlushError, true);
			m_projects.CaptureProjectPreview();
		}
		else if (saved)
		{
			ShowToast(std::string(ICON_FA_FLOPPY_DISK "  Saved  ") + currentName);
			m_projects.CaptureProjectPreview();
		}
		else if (m_hierarchyPanel != nullptr)
		{
			m_hierarchyPanel->RequestSaveAsPopup();
		}
		return saved;
	}

	void DebugLayer::ApplyHistoryStep(app::LayerContext& context, const bool redo)
	{
		IEditorCommand* command = redo ? m_undoStack.Redo(context.Get<World>(), context.services) : m_undoStack.Undo(context.Get<World>(), context.services);
		if (command == nullptr)
		{
			return;
		}
		// Preserve the selection across the edit: remap each id through the command
		// (identity unless it recreated entities), then drop any that no longer exist.
		World& world = context.Get<World>();
		std::vector<Entity> remapped = m_selection.All();
		for (Entity& e: remapped)
		{
			e = command->Remap(e);
		}
		const Entity primary = command->Remap(m_selection.Primary());
		m_selection.Replace(std::move(remapped), primary);
		m_selection.Prune(world);
	}

	bool DebugLayer::HasUnsavedWork() const
	{
		// Tilemap cells are a second document: they live in their own .tiles asset, so a
		// scene whose entity history is clean can still hold unsaved paint.
		return m_undoStack.HasUnsavedChanges() || m_tilePainting.mapDirty;
	}

	std::vector<std::string_view> DebugLayer::UnsavedDocuments(app::LayerContext& context) const
	{
		// Deliberately separate from HasUnsavedWork: a half-edited material is not the scene,
		// and folding it in would make the status bar claim the scene needs saving.
		std::vector<std::string_view> names;
		for (const std::unique_ptr<DebugPanel>& panel: m_panels)
		{
			if (panel != nullptr && panel->HasUnsavedWork(context))
			{
				names.push_back(panel->GetName());
			}
		}
		return names;
	}

	void DebugLayer::ConfirmDiscard(app::LayerContext& context, const PendingNav nav)
	{
		m_pendingNav = nav;
		const bool documentsDirty = (nav == PendingNav::CloseEditor) && !UnsavedDocuments(context).empty();
		if (!HasUnsavedWork() && !documentsDirty)
		{
			RunPendingNav(context);
			return;
		}
		m_openUnsavedPopup = true;
	}

	void DebugLayer::RunPendingNav(app::LayerContext& context)
	{
		const PendingNav nav = m_pendingNav;
		m_pendingNav = PendingNav::None;
		switch (nav)
		{
			case PendingNav::None:
				return;
			case PendingNav::NewScene3D:
			case PendingNav::NewScene2D:
			{
				const SceneKind kind = (nav == PendingNav::NewScene3D) ? SceneKind::Scene3D : SceneKind::Scene2D;
				const std::string name = app::scene::NewScene(context.Get<World>(), app::scene::MakeApplySceneDeps(context.services), kind);
				if (name.empty())
				{
					return;
				}
				if (auto* scenes = context.TryGet<SceneSubsystem>())
				{
					scenes->SetCurrentScene("");
				}
				m_selection.Clear();
				// The document was replaced: the old scene's history addresses entities that
				// no longer exist, and the blank scene starts clean.
				editor::ResetEditHistory(context.services);
				m_tilePainting.mapDirty = false;
				return;
			}
			case PendingNav::OpenScene:
				if (m_hierarchyPanel != nullptr)
				{
					m_hierarchyPanel->RequestOpenPopup();
				}
				return;
			case PendingNav::CloseEditor:
				if (auto* window = context.services.TryGet<Window>())
				{
					window->RequestClose();
				}
				return;
		}
	}

	void DebugLayer::PollCloseRequest(app::LayerContext& context)
	{
		auto* window = context.services.TryGet<Window>();
		if (window == nullptr || !window->ShouldClose())
		{
			return; // nothing pending - let the loop exit
		}
		if (!HasUnsavedWork() && UnsavedDocuments(context).empty())
		{
			return; // nothing to lose
		}
		// Withdraw the OS request and ask instead. Events are pumped before layers update
		// and the loop only re-reads the flag next iteration, so clearing it here keeps the
		// window alive long enough for the answer.
		window->CancelClose();
		ConfirmDiscard(context, PendingNav::CloseEditor);
	}

	void DebugLayer::DrawUnsavedChangesPopup(app::LayerContext& context)
	{
		constexpr const char* kTitle = "Unsaved Changes###unsavedChanges";
		if (m_openUnsavedPopup)
		{
			ImGui::OpenPopup(kTitle);
			m_openUnsavedPopup = false;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		std::string sceneName;
		if (const auto* scenes = context.TryGet<SceneSubsystem>(); scenes != nullptr)
		{
			sceneName = scenes->GetCurrentScene();
		}
		const char* verb = (m_pendingNav == PendingNav::CloseEditor) ? "Closing the editor" : "Opening another scene";
		if (m_pendingNav == PendingNav::NewScene2D || m_pendingNav == PendingNav::NewScene3D)
		{
			verb = "Starting a new scene";
		}
		const std::vector<std::string_view> documents = UnsavedDocuments(context);
		const bool sceneDirty = HasUnsavedWork();
		if (sceneDirty)
		{
			ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  %s has unsaved changes.", sceneName.empty() ? "This scene" : sceneName.c_str());
		}
		else
		{
			ImGui::TextUnformatted(ICON_FA_TRIANGLE_EXCLAMATION "  You have unsaved changes.");
		}
		ImGui::PushStyleColor(ImGuiCol_Text, chrome::kMuted);
		// Naming the editors matters: their windows may be closed, so "unsaved changes" alone
		// gives no clue what is about to go.
		for (const std::string_view document: documents)
		{
			ImGui::Text("    %s has unsaved edits.", std::string(document).c_str());
		}
		ImGui::Text("%s will discard them.", verb);
		ImGui::PopStyleColor();
		ImGui::Spacing();

		// Save is the default: Enter and Escape are the two keys people hit reflexively, so
		// the safe action takes Enter and the reversible one takes Escape. Nothing here
		// discards without a deliberate click.
		if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK "  Save", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter))
		{
			// Every dirty document too, or Save would close the editor having written only
			// the scene and dropped the rest.
			bool documentsSaved = true;
			for (const std::unique_ptr<DebugPanel>& panel: m_panels)
			{
				if (panel != nullptr && panel->HasUnsavedWork(context))
				{
					documentsSaved = panel->SaveUnsavedWork(context) && documentsSaved;
				}
			}
			if (!documentsSaved)
			{
				// A write failed. Stay open rather than close over work that is still only
				// in memory; the panel's own status line says which one.
				m_pendingNav = PendingNav::None;
				ImGui::CloseCurrentPopup();
			}
			else if (!sceneDirty)
			{
				ImGui::CloseCurrentPopup();
				RunPendingNav(context);
			}
			else if (SaveCurrentScene(context))
			{
				ImGui::CloseCurrentPopup();
				RunPendingNav(context);
			}
			else
			{
				// An unnamed scene routes to Save As instead; abandon the navigation rather
				// than run it behind the dialog the user now has to answer.
				m_pendingNav = PendingNav::None;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (chrome::OutlineButton("Discard", ImVec2(110.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			RunPendingNav(context);
		}
		ImGui::SameLine();
		if (chrome::OutlineButton("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			m_pendingNav = PendingNav::None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void DebugLayer::PollRecoveryOffer(app::LayerContext& context)
	{
		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			return;
		}
		if (project->root == m_recoveryCheckedRoot)
		{
			return; // already asked for this project
		}
		m_recoveryCheckedRoot = project->root;
		m_recoveryError.clear();

		// Touches the disk, so it runs once per project open rather than per frame. Empty
		// is the normal case: a clean save discards the copy it made redundant.
		m_recoverable = editor::AutosaveService::FindRecoverable(*project);
		m_openRecoveryPopup = !m_recoverable.empty();
	}

	void DebugLayer::DrawRecoveryPopup(app::LayerContext& context)
	{
		constexpr const char* kTitle = "Recover Unsaved Work###recoverScenes";
		if (m_openRecoveryPopup)
		{
			ImGui::OpenPopup(kTitle);
			m_openRecoveryPopup = false;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(720.0f, viewport->WorkSize.y * 0.8f));
		if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded() || m_recoverable.empty())
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  Autosave has newer work for %d scene%s.", static_cast<int>(m_recoverable.size()), m_recoverable.size() == 1 ? "" : "s");
		ImGui::PushStyleColor(ImGuiCol_Text, chrome::kMuted);
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
		ImGui::TextUnformatted("These are recovery copies written while the editor was running. Your saved scenes are untouched until you choose Restore.");
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::Spacing();

		auto* scenes = context.TryGet<SceneSubsystem>();
		const std::string currentScene = scenes != nullptr ? scenes->GetCurrentScene() : std::string{};

		std::string handled;
		bool restored = false;
		for (const RecoveredScene& recovered: m_recoverable)
		{
			ImGui::PushID(recovered.sceneName.c_str());
			ImGui::AlignTextToFramePadding();
			ImGui::Text(ICON_FA_CUBE "  %s", recovered.sceneName.c_str());
			ImGui::SameLine();
			// When the copy was WRITTEN, not the gap between it and the last save. That gap
			// is how long you had gone without saving, and reading it as "3 weeks ahead of
			// the saved scene" says three weeks of work are at stake when none may be.
			const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - recovered.savedAt).count();
			ImGui::TextColored(chrome::kMuted, "autosaved %s ago", utils::DurationLabel(age).c_str());

			ImGui::SameLine(ImGui::GetContentRegionMax().x - 190.0f);
			if (chrome::PrimaryButton("Restore", ImVec2(90.0f, 0.0f)))
			{
				std::string error;
				if (editor::AutosaveService::Restore(*project, recovered.sceneName, error))
				{
					handled = recovered.sceneName;
					restored = true;
				}
				else
				{
					// Restore refuses a copy that does not parse, rather than destroying a
					// stale-but-valid scene with a broken one. Say so instead of silently
					// leaving the row in place.
					m_recoveryError = "Could not restore '" + recovered.sceneName + "': " + error;
				}
			}
			ImGui::SameLine();
			if (chrome::OutlineButton("Discard", ImVec2(90.0f, 0.0f)))
			{
				editor::AutosaveService::Discard(*project, recovered.sceneName);
				handled = recovered.sceneName;
			}
			ImGui::PopID();
		}

		if (!m_recoveryError.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::kError);
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
			ImGui::TextUnformatted(m_recoveryError.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}

		if (!handled.empty())
		{
			std::erase_if(m_recoverable, [&handled](const RecoveredScene& entry) { return entry.sceneName == handled; });
			// Restoring the scene that is already open replaces the file under it, so pull
			// the new contents in - otherwise the editor keeps showing the version that was
			// just overwritten and a save would put it straight back.
			if (restored && handled == currentScene)
			{
				if (app::scene::LoadSceneFile(handled, context.Get<World>(), app::scene::MakeApplySceneDeps(context.services)))
				{
					m_selection.Clear();
					editor::ResetEditHistory(context.services);
					m_tilePainting.mapDirty = false;
				}
			}
			if (restored)
			{
				ShowToast(std::string(ICON_FA_ROTATE_LEFT "  Restored  ") + handled);
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		// Closing without choosing leaves every copy exactly where it is, so the offer
		// comes back next time the project opens. Nothing here is destructive by default.
		if (m_recoverable.empty() || chrome::OutlineButton("Decide later", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void DebugLayer::SaveAndReturnToLauncher(app::LayerContext& context)
	{
		if (!SaveCurrentScene(context))
		{
			ShowToast(ICON_FA_CIRCLE_INFO "  Save the scene before returning to the launcher.", true);
			return;
		}
		// The save is asynchronous; the launcher reads the project's scene files, so
		// block until the write lands on disk before handing off.
		m_sceneWriter.Flush();

		// A running editor never starts a second process. It already contains the launcher:
		// EditorProjectManager draws the same screen, and its OpenProject remounts the VFS,
		// rebuilds scripts and loads the startup scene in place - the path the Project
		// panel's Launcher button has always used.
		//
		// This used to spawn Launcher.exe and exit. That threw away a warm process (engine
		// init, shader overlay, compiled scripts, window placement) to show a screen this
		// one can already draw, and it silently ended any debug session attached to it -
		// the replacement process is a new one, undebugged.
		//
		// Launcher.exe is still the entry point and still spawns editors; only the reverse
		// direction is gone.
		m_projects.OpenLauncher();
	}

	void DebugLayer::CaptureEditorWindowSize(app::LayerContext& context)
	{
		auto* window = context.services.TryGet<Window>();
		if (window == nullptr)
		{
			return;
		}
		// launcher-size exclusion - the launcher never resizes the OS window anymore.
		const auto size = window->GetWindowSize();
		if (size.width >= 640 && size.height >= 480 && (size.width != m_editorWindowW || size.height != m_editorWindowH))
		{
			m_editorWindowW = size.width;
			m_editorWindowH = size.height;
		}
	}

	void DebugLayer::ShowToast(std::string text, bool isError)
	{
		m_toastText = std::move(text);
		m_toastStart = ImGui::GetTime();
		m_toastError = isError;
	}

	void DebugLayer::DrawToasts()
	{
		if (m_toastStart < 0.0)
		{
			return;
		}
		constexpr float kLifetime = 2.4f;
		constexpr float kFadeIn = 0.12f;
		constexpr float kFadeOut = 0.5f;
		const auto age = static_cast<float>(ImGui::GetTime() - m_toastStart);
		if (age > kLifetime)
		{
			m_toastStart = -1.0;
			return;
		}

		float alpha = 1.0f;
		if (age < kFadeIn)
		{
			alpha = age / kFadeIn;
		}
		else if (age > kLifetime - kFadeOut)
		{
			alpha = (kLifetime - age) / kFadeOut;
		}
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
		constexpr float kFont = 14.0f;
		const ImVec2 textSize = chrome::MeasureSized(kFont, m_toastText.c_str());
		constexpr float padX = 18.0f;
		constexpr float padY = 10.0f;
		const float w = textSize.x + padX * 2.0f;
		const float h = textSize.y + padY * 2.0f;
		const float rise = (1.0f - alpha) * 8.0f;
		const float cx = vp->Pos.x + vp->Size.x * 0.5f;
		const float bottom = vp->Pos.y + vp->Size.y - ImGui::GetFrameHeight() - 18.0f + rise;
		const ImVec2 p0(cx - w * 0.5f, bottom - h);
		const ImVec2 p1(cx + w * 0.5f, bottom);
		const ImVec4 accent = m_toastError ? chrome::kError : chrome::kSuccess;

		constexpr float rounding = 9.0f;
		constexpr ImDrawFlags roundRight = ImDrawFlags_RoundCornersRight;
		dl->AddRectFilled(ImVec2(p0.x, p0.y + 3.0f), ImVec2(p1.x, p1.y + 3.0f), chrome::U32(chrome::WithAlpha(chrome::kBg, 0.55f * alpha)), rounding, roundRight);
		dl->AddRectFilled(p0, p1, chrome::U32(chrome::WithAlpha(chrome::kPanelHi, 0.98f * alpha)), rounding, roundRight);
		dl->AddRect(p0, p1, chrome::U32(chrome::WithAlpha(accent, 0.75f * alpha)), rounding, roundRight, 1.5f);
		dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f, p1.y), chrome::U32(chrome::WithAlpha(accent, alpha)));
		chrome::TextSized(dl, kFont, ImVec2(p0.x + padX, p0.y + padY), chrome::WithAlpha(chrome::kText, alpha), m_toastText.c_str());
	}

	void DebugLayer::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		CaptureEditorWindowSize(context);

		// Before anything draws: the OS close request arrives during the event pump earlier
		// this frame, and the loop re-reads it after the layers run.
		PollCloseRequest(context);
		PollRecoveryOffer(context);

		ImGuizmo::BeginFrame();

		// Applied here rather than where the request was made: ImGui only records a focus
		// against a live frame, and the control command that asked for it was drained before
		// this one started.
		if (!m_pendingFocusWindow.empty())
		{
			ImGui::SetWindowFocus(m_pendingFocusWindow.c_str());
			m_pendingFocusWindow.clear();
		}

		// A layout preset queued last frame is applied here, before any window
		if (m_pendingLayoutApply)
		{
			ImGui::LoadIniSettingsFromMemory(m_pendingLayoutIni.c_str(), m_pendingLayoutIni.size());
			for (const auto& [name, visible]: m_pendingLayoutVisibility)
			{
				if (DebugPanel* panel = FindPanelByName(name))
				{
					panel->SetVisible(visible);
				}
			}
			m_pendingLayoutApply = false;
		}

		if (!m_projects.IsProjectLoaded() || m_projects.IsLauncherOpen())
		{
			if (m_dockspaceBuilt)
			{
				ImGuiViewport* mainViewport = ImGui::GetMainViewport();
				const ImU32 backdrop = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32(0, 0, 0, 255);
				ImGui::GetBackgroundDrawList(mainViewport)->AddRectFilled(mainViewport->Pos, ImVec2(mainViewport->Pos.x + mainViewport->Size.x, mainViewport->Pos.y + mainViewport->Size.y), backdrop);
			}
			m_projects.DrawLauncher();
			PersistSettings(context);
			return;
		}

		if (m_dockspaceBuilt)
		{
			ImGuiViewport* mainViewport = ImGui::GetMainViewport();
			const ImU32 editorBg = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32(0, 0, 0, 255);
			ImGui::GetBackgroundDrawList(mainViewport)->AddRectFilled(mainViewport->Pos, ImVec2(mainViewport->Pos.x + mainViewport->Size.x, mainViewport->Pos.y + mainViewport->Size.y), editorBg);
		}

		const auto* undoPlayState = context.TryGet<app::PlayState>();
		const bool undoEditable = undoPlayState != nullptr && !undoPlayState->IsPlaying() && !undoPlayState->IsCompiling();
		if (!undoEditable)
		{
			// Compiling/playing: the scene is the running sim, not an editable doc.
			m_undoStack.AbandonPending();
			// Drop queued control-endpoint steps too, rather than replaying them into
			// whatever scene is loaded once editing resumes.
			m_pendingUndoSteps = 0;
			m_pendingRedoSteps = 0;
			m_pendingOpenSceneDialog = false;
		}
		if (undoEditable)
		{
			const ImGuiIO& io = ImGui::GetIO();
			if (io.KeyCtrl && !io.WantTextInput)
			{
				const bool zKey = ImGui::IsKeyPressed(shortcuts::kUndo.key, false);
				const bool redoCombo = ImGui::IsKeyPressed(shortcuts::kRedo.key, false) || (zKey && io.KeyShift);
				const bool undoCombo = zKey && !io.KeyShift;
				if (undoCombo || redoCombo)
				{
					// A material is its own document with its own history, the same reason
					// Ctrl+S saves the focused material rather than the scene. Undoing the
					// scene while looking at a half-edited graph is the surprising answer.
					auto* material = dynamic_cast<MaterialGraphPanel*>(FindPanelByName("Material"));
					bool handled = material != nullptr && (redoCombo ? material->RedoIfFocused() : material->UndoIfFocused());
					if (!handled)
					{
						// A pixel canvas is a document too, and it had only toolbar buttons:
						// Ctrl+Z over it left the artwork alone and undid a scene edit made
						// earlier instead, which is the worst of both answers.
						auto* pixels = dynamic_cast<PixelArtPanel*>(FindPanelByName("Pixel Art"));
						handled = pixels != nullptr && (redoCombo ? pixels->RedoIfFocused(context) : pixels->UndoIfFocused(context));
					}
					if (!handled)
					{
						ApplyHistoryStep(context, redoCombo);
					}
				}
			}
			if (m_pendingOpenSceneDialog)
			{
				m_pendingOpenSceneDialog = false;
				ConfirmDiscard(context, PendingNav::OpenScene);
			}
			for (; m_pendingUndoSteps > 0; --m_pendingUndoSteps)
			{
				ApplyHistoryStep(context, false);
			}
			for (; m_pendingRedoSteps > 0; --m_pendingRedoSteps)
			{
				ApplyHistoryStep(context, true);
			}
		}

		{
			const ImGuiIO& io = ImGui::GetIO();
			if (!io.WantTextInput && ImGui::IsKeyPressed(shortcuts::kShortcuts.key, false))
			{
				m_openShortcuts = true;
			}
			// Scene editing keys deliberately work from anywhere, so a document panel that
			// binds them for its own contents has to say so or Delete hits both.
			if (m_hierarchyPanel != nullptr)
			{
				bool documentOwnsKeys = false;
				for (const auto& panel: m_panels)
				{
					documentOwnsKeys = documentOwnsKeys || panel->OwnsEditingKeys();
				}
				m_hierarchyPanel->SetDocumentPanelOwnsEditingKeys(documentOwnsKeys);
			}

			if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(shortcuts::kSave.key, false))
			{
				// Shift is the Save As modifier everywhere else, and this used to ignore it -
				// so Ctrl+Shift+S silently overwrote the current scene.
				if (io.KeyShift)
				{
					if (m_hierarchyPanel != nullptr)
					{
						m_hierarchyPanel->RequestSaveAsPopup();
					}
				}
				else
				{
					// Save what is being edited. With the Material window focused over a
					// half-edited material, saving the scene instead is both surprising and
					// leaves the thing you were working on unsaved.
					auto* material = dynamic_cast<MaterialGraphPanel*>(FindPanelByName("Material"));
					auto* pixels = dynamic_cast<PixelArtPanel*>(FindPanelByName("Pixel Art"));
					const bool savedDocument = (material != nullptr && material->SaveIfFocusedAndDirty(context)) || (pixels != nullptr && pixels->SaveIfFocusedAndDirty(context));
					if (!savedDocument)
					{
						SaveCurrentScene(context);
					}
				}
			}
		}

		// Play-control shortcuts (act on the running sim, so not gated on edit mode).
		// F6 toggles pause, F7 steps one frame. Both no-op unless a session is live.
		{
			const ImGuiIO& io = ImGui::GetIO();
			if (!io.WantTextInput)
			{
				if (ImGui::IsKeyPressed(shortcuts::kPausePlay.key, false))
				{
					TogglePausePlaySession(context);
				}
				if (ImGui::IsKeyPressed(shortcuts::kStepFrame.key, false))
				{
					StepPlaySession(context);
				}
			}
		}

		if (m_scriptErrors.Draw())
		{
			m_pendingFocusWindow = "Console";
			if (auto* console = dynamic_cast<ConsolePanel*>(FindPanelByName("Console")))
			{
				console->SetVisible(true);
				console->ShowScriptErrors();
			}
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		const bool showMenuBar = m_dockspaceBuilt;
		const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus
		                                   | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground | (showMenuBar ? ImGuiWindowFlags_MenuBar : 0);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DebugDockSpace", nullptr, hostFlags);
		ImGui::PopStyleVar(3);

		ImGui::PushStyleColor(ImGuiCol_MenuBarBg, chrome::kPanel);
		ImGui::PushStyleColor(ImGuiCol_Header, chrome::WithAlpha(chrome::kAccent, 0.20f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, chrome::WithAlpha(chrome::kAccent, 0.28f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, chrome::WithAlpha(chrome::kAccent, 0.36f));
		if (showMenuBar && ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				// No trailing "...": this no longer leaves for another process, it saves and
				// shows the launcher over this editor. Esc there brings the project back.
				if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Save & Return to Project Launcher"))
				{
					SaveAndReturnToLauncher(context);
				}
				ImGui::Separator();
				// Everything that replaces the scene in memory goes through ConfirmDiscard.
				// These used to run straight through: one click on New Scene threw away an
				// unsaved session with no prompt and no way back.
				if (ImGui::MenuItem(ICON_FA_PLUS "  New 3D Scene"))
				{
					ConfirmDiscard(context, PendingNav::NewScene3D);
				}
				if (ImGui::MenuItem(ICON_FA_PLUS "  New 2D Scene"))
				{
					ConfirmDiscard(context, PendingNav::NewScene2D);
				}
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open..."))
				{
					ConfirmDiscard(context, PendingNav::OpenScene);
				}
				if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save", shortcuts::kSave.display))
				{
					SaveCurrentScene(context);
				}
				if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save As...", shortcuts::kSaveAs.display))
				{
					if (m_hierarchyPanel != nullptr)
					{
						m_hierarchyPanel->RequestSaveAsPopup();
					}
				}
				ImGui::Separator();
				// Publishing is the end of the pipeline and had no entry point here at all: the
				// Build panel is hidden by default and lives inside a Window submenu, so shipping
				// a game meant already knowing where the button was. This is where anyone would
				// look for it first.
				if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Publish..."))
				{
					ShowPanel("Build");
				}
				ImGui::EndMenu();
			}
			// A full undo/redo stack existed and was reachable only by knowing the keys.
			// Nothing on screen said so, and nothing showed whether there was anything to
			// undo - which is most of what a menu entry is for.
			if (ImGui::BeginMenu("Edit"))
			{
				if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Undo", "Ctrl+Z", false, undoEditable && m_undoStack.UndoDepth() > 0))
				{
					ApplyHistoryStep(context, false);
				}
				if (ImGui::MenuItem(ICON_FA_ROTATE_RIGHT "  Redo", "Ctrl+Y", false, undoEditable && m_undoStack.RedoDepth() > 0))
				{
					ApplyHistoryStep(context, true);
				}
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Command Palette...", "Ctrl+P"))
				{
					m_openCommandPalette = true;
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Window"))
			{
				auto windowToggle = [this](std::string_view name)
				{
					for (auto& panel: m_panels)
					{
						if (panel->GetName() == name)
						{
							const std::string label = std::string(WindowMenuIcon(name)) + "  " + std::string(name);
							ImGui::MenuItem(label.c_str(), nullptr, panel->VisiblePtr());
							return;
						}
					}
				};

				struct MenuGroup
				{
					const char* icon;
					const char* label;
					std::vector<std::string_view> windows;
				};

				static const std::vector<MenuGroup> kGroups = {
				        {ICON_FA_CUBE, "Scene", {"Scene", "Project", "Build", "File Explorer", "Inspector", "Viewport", "UI Canvas"}},
				        {ICON_FA_BRUSH, "Authoring", {"Material", "Textures", "Sprite Slicer", "Sprite Animation", "Tile Palette", "Pixel Art"}},
				        {ICON_FA_PALETTE, "Rendering", {"Render Graph", "Post Processing", "Particles"}},
				        {ICON_FA_GAUGE_HIGH, "Diagnostics", {"Performance", "Console", "Dev Tools", "Control Server"}},
				        {ICON_FA_GEARS, "Engine", {"Settings", "Theme"}},
				};

				std::unordered_set<std::string_view> grouped;
				for (const auto& group: kGroups)
				{
					for (const auto& name: group.windows)
					{
						grouped.insert(name);
					}
				}

				for (const auto& group: kGroups)
				{
					const std::string groupLabel = std::string(group.icon) + "  " + group.label;
					if (ImGui::BeginMenu(groupLabel.c_str()))
					{
						for (const auto& name: group.windows)
						{
							windowToggle(name);
						}
						ImGui::EndMenu();
					}
				}

				// Safety net: any panel not assigned to a group still gets a toggle so
				const bool hasUngrouped = std::ranges::any_of(m_panels, [&](const auto& panel) { return !grouped.contains(panel->GetName()); });
				if (hasUngrouped && ImGui::BeginMenu(ICON_FA_CIRCLE "  Other"))
				{
					for (auto& panel: m_panels)
					{
						if (!grouped.contains(panel->GetName()))
						{
							windowToggle(panel->GetName());
						}
					}
					ImGui::EndMenu();
				}

				ImGui::Separator();
				if (ImGui::MenuItem("Show All Windows"))
				{
					for (auto& panel: m_panels)
					{
						panel->SetVisible(true);
					}
				}
				if (ImGui::MenuItem("Hide All Windows"))
				{
					for (auto& panel: m_panels)
					{
						panel->SetVisible(false);
					}
				}
				ImGui::Separator();
				if (ImGui::BeginMenu("Layouts"))
				{
					ImGui::TextDisabled("Workflows");
					for (const WorkflowLayoutDef& wf: kWorkflowLayouts)
					{
						if (ImGui::MenuItem(wf.name))
						{
							ApplyWorkflowLayout(static_cast<int>(wf.id));
						}
						if (ImGui::IsItemHovered() && (wf.tooltip != nullptr))
						{
							ImGui::SetTooltip("%s", wf.tooltip);
						}
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Save Current As..."))
					{
						m_openSavePresetPopup = true;
					}
					ImGui::Separator();
					ImGui::TextDisabled("Saved");
					if (m_layoutPresets.empty())
					{
						ImGui::TextDisabled("(no saved layouts)");
					}
					for (const auto& preset: m_layoutPresets)
					{
						if (ImGui::MenuItem(preset.name.c_str()))
						{
							ApplyLayoutPreset(preset);
						}
					}
					if (!m_layoutPresets.empty())
					{
						ImGui::Separator();
						if (ImGui::BeginMenu("Delete"))
						{
							for (const auto& preset: m_layoutPresets)
							{
								if (ImGui::MenuItem(preset.name.c_str()))
								{
									DeleteLayoutPreset(preset.name);
								}
							}
							ImGui::EndMenu();
						}
					}
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Reset Layout"))
				{
					m_resetLayout = true;
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("View"))
			{
				bool debugRendering = IsDebugRenderingEnabled();
				if (ImGui::MenuItem("Debug Rendering", nullptr, &debugRendering))
				{
					SetDebugRenderingEnabled(debugRendering);
				}
				ImGui::EndMenu();
			}

			// Project menu: show/hide the debug panels the loaded project registered via IEditorWindow.
			// Fully generic - the editor enumerates + toggles whatever the project exposes, knowing none
			// of them. Only appears when the project actually registered at least one window.
			if (const auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>())
			{
				const auto* api = scripting->Api();
				const int windowCount = (api != nullptr && api->GetEditorWindowCount != nullptr) ? api->GetEditorWindowCount() : 0;
				if (windowCount > 0 && ImGui::BeginMenu("Project"))
				{
					ImGui::TextDisabled("Debug Panels");
					for (int i = 0; i < windowCount; ++i)
					{
						char title[128] = {};
						if (api->GetEditorWindowTitle != nullptr)
						{
							api->GetEditorWindowTitle(i, title, static_cast<std::int32_t>(sizeof(title)));
						}
						bool visible = api->GetEditorWindowVisible != nullptr && api->GetEditorWindowVisible(i) != 0;
						if (ImGui::MenuItem(title[0] != '\0' ? title : "(window)", nullptr, &visible) && api->SetEditorWindowVisible != nullptr)
						{
							api->SetEditorWindowVisible(i, visible ? 1 : 0);
						}
					}
					if (api->SetEditorWindowVisible != nullptr)
					{
						ImGui::Separator();
						if (ImGui::MenuItem("Show All"))
						{
							for (int i = 0; i < windowCount; ++i)
							{
								api->SetEditorWindowVisible(i, 1);
							}
						}
						if (ImGui::MenuItem("Hide All"))
						{
							for (int i = 0; i < windowCount; ++i)
							{
								api->SetEditorWindowVisible(i, 0);
							}
						}
					}
					ImGui::EndMenu();
				}
			}

			{
				using namespace chrome;
				// Somewhere to go when you are stuck. The engine ships a getting-started walkthrough
				// and until now nothing in the editor mentioned it existed.
				if (ImGui::BeginMenu("Help"))
				{
					const std::filesystem::path guide = FindDocsFile("getting-started.md");
					if (ImGui::MenuItem(ICON_FA_BOOK "  Getting Started", nullptr, false, !guide.empty()))
					{
						OpenPathInShell(guide);
					}
					if (guide.empty() && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("docs/getting-started.md was not found next to the editor.");
					}
					const std::filesystem::path docs = FindDocsFile({});
					if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Documentation Folder", nullptr, false, !docs.empty()))
					{
						OpenPathInShell(docs);
					}
					ImGui::EndMenu();
				}
				const auto* playState = context.TryGet<app::PlayState>();
				const bool compiling = playState != nullptr && playState->IsCompiling();
				const char* chip = compiling ? ICON_FA_HAMMER "  BUILD" : "AETHERCORE";
				const ImVec4 chipColor = compiling ? kAccentHi : kFaint;
				const float chipW = ImGui::CalcTextSize(chip).x;
				const float avail = ImGui::GetContentRegionAvail().x;
				if (avail > chipW + 16.0f)
				{
					ImGui::SameLine(ImGui::GetCursorPosX() + avail - chipW - 12.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, chipColor);
					ImGui::TextUnformatted(chip);
					ImGui::PopStyleColor();
				}
			}
			ImGui::EndMenuBar();
		}
		ImGui::PopStyleColor(4);

		if (m_openSavePresetPopup)
		{
			ImGui::OpenPopup("Save Layout##popup");
			m_newPresetName[0] = '\0';
			m_openSavePresetPopup = false;
		}
		if (ImGui::BeginPopupModal("Save Layout##popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Preset name");
			if (ImGui::IsWindowAppearing())
			{
				ImGui::SetKeyboardFocusHere();
			}
			const bool entered = ImGui::InputText("##presetname", m_newPresetName, sizeof(m_newPresetName), ImGuiInputTextFlags_EnterReturnsTrue);
			const bool hasName = m_newPresetName[0] != '\0';
			ImGui::BeginDisabled(!hasName);
			if ((ImGui::Button("Save") || entered) && hasName)
			{
				CaptureCurrentLayout(m_newPresetName);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		const ImGuiID dockspace_id = ImGui::GetID("AetherDebugDockSpaceV6");
		const bool hasSavedDockspace = ImGui::DockBuilderGetNode(dockspace_id) != nullptr;
		const bool showStatusBar = m_dockspaceBuilt;
		const float statusBarHeight = showStatusBar ? ImGui::GetFrameHeight() : 0.0f;
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
		ImGui::DockSpace(dockspace_id, ImVec2(0.0f, -statusBarHeight), ImGuiDockNodeFlags_PassthruCentralNode);

		if (m_resetLayout || (!m_dockspaceBuilt && !hasSavedDockspace))
		{
			const WorkflowLayout layout = (m_pendingWorkflowLayout >= 0 && m_pendingWorkflowLayout < static_cast<int>(WorkflowLayout::Count)) ? static_cast<WorkflowLayout>(m_pendingWorkflowLayout) : WorkflowLayout::Default;
			// Every panel the editor owns, so the builder can give a home to the ones this
			// layout says nothing about.
			std::vector<std::string_view> panelNames;
			panelNames.reserve(m_panels.size());
			for (const auto& panel: m_panels)
			{
				panelNames.push_back(panel->GetWindowTitle());
			}
			BuildWorkflowLayout(layout, dockspace_id, viewport->WorkSize, panelNames);
			m_focusViewportAfterLayout = true;
		}
		m_dockspaceBuilt = true;
		m_resetLayout = false;
		m_pendingWorkflowLayout = -1;

		if (showStatusBar)
		{
			DrawStatusBar(context);
			DrawToasts();
		}
		ImGui::PopStyleVar();

		ImGui::End();

		// Panels Begin their own window, so this is the one place that can bound all of
		// them. Undocked, a panel becomes its own OS window (multi-viewport is on), and a
		// window with no size hint auto-fits to its content - which with wrap-at-window
		// text is a feedback loop that settles narrow and very tall, because the wrap
		// width is measured against the width the window already has. File > Publish
		// opened 290x2082 on a 1440px display, most of it off-screen.
		//
		// Both calls are FirstUseEver/advisory and are issued BEFORE OnImGui, so a panel
		// with its own considered size (Sprite Slicer, Tile Palette) still wins - the last
		// call before Begin is the one that counts.
		ImVec2 maxPanelSize = ImGui::GetMainViewport()->WorkSize;
		for (const ImGuiPlatformMonitor& monitor: ImGui::GetPlatformIO().Monitors)
		{
			// Largest attached work area, so dragging a panel to a bigger second display
			// is not clamped to the size of the one the editor happens to sit on.
			maxPanelSize.x = std::max(maxPanelSize.x, monitor.WorkSize.x);
			maxPanelSize.y = std::max(maxPanelSize.y, monitor.WorkSize.y);
		}
		const ImVec2 defaultPanelSize(std::min(720.0f, maxPanelSize.x * 0.6f), std::min(560.0f, maxPanelSize.y * 0.6f));
		for (auto& panel: m_panels)
		{
			if (panel->IsVisible())
			{
				ImGui::SetNextWindowSize(defaultPanelSize, ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 120.0f), maxPanelSize);
				panel->OnImGui(context);
			}
		}

		// Every layout docks the Viewport to the centre node, and most dock tool panels
		// there beside it. Docking does not decide which tab is SELECTED: without this the
		// editor came up on whichever tool sorted first in the panel list - Default opened
		// showing the sprite atlas editor with the game hidden one tab over. This has to
		// run AFTER the panels have drawn, because at DockBuilderFinish time none of the
		// windows in the node exist yet for the frame and the focus does not stick.
		if (m_focusViewportAfterLayout)
		{
			ImGui::SetWindowFocus("Viewport");
			m_focusViewportAfterLayout = false;
		}

		// Project-defined editor windows (IEditorWindow): pump their ImGui after the built-in panels.
		// Generic hook - the editor knows nothing about what any project draws. Each window Begins/Ends
		// itself; DrawEditorWindows isolates a throwing window so it can't crash the editor.
		if (const auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>())
		{
			if (const auto* api = scripting->Api(); api != nullptr && api->DrawEditorWindows != nullptr)
			{
				api->DrawEditorWindows();
			}
		}

		DrawCommandPalette(context);
		DrawShortcutsReference();
		DrawUnsavedChangesPopup(context);
		DrawRecoveryPopup(context);

		// An inspector field edit ends when its widget stops being active - a slider
		// drag and a focused text box both stay active across frames, so this is what
		// turns the whole interaction into one command rather than one per frame.
		if (undoEditable && !ImGui::IsAnyItemActive())
		{
			m_undoStack.FlushFieldEdit();
		}

		PersistSettings(context);
	}

	void DebugLayer::ShowPanel(const std::string_view name)
	{
		for (auto& panel: m_panels)
		{
			if (panel->GetName() == name)
			{
				if (bool* visible = panel->VisiblePtr(); visible != nullptr)
				{
					*visible = true;
				}
				ImGui::SetWindowFocus(std::string(name).c_str());
				return;
			}
		}
	}
} // namespace aether::editor
