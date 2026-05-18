#include "ScriptedSceneLayer.hpp"

#include <array>
#include <filesystem>
#include <span>

#include "scripting/ScriptingSubsystem.hpp"

#include "assets/AssetManager.hpp"
#include "camera/CameraManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/Input.hpp"
#include "rendering/LightingManager.hpp"
#include "rendering/Renderer.hpp"
#include "gpu/GpuDevice.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	ScriptedSceneLayer::ScriptedSceneLayer(std::string scriptPath, SystemFactory systemFactory)
	      : m_scriptPath(std::move(scriptPath)), m_systemFactory(std::move(systemFactory))
	{
	}

	// ── Helpers ───────────────────────────────────────────────────────────────

	void ScriptedSceneLayer::BuildDefaultPipeline(LayerContext& context)
	{
		auto& assets = context.Get<AssetManager>();
		const auto bindlessLayout = context.Get<BindlessManager>().GetLayout();
		const auto lightingLayout = context.Get<LightingManager>().GetSetLayout();
		const std::array<VkDescriptorSetLayout, 2> setLayouts{ bindlessLayout, lightingLayout };

		auto result = assets.CreateGraphicsPipeline({
		        .shaderVfsPath = "shaders://gltf_mesh.slang.spv",
		        .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		        .depthFormat = context.Get<Swapchain>().GetDepthFormat(),
		        .depthTestEnable = true,
		        .depthWriteEnable = true,
		        .setLayouts = std::span<const VkDescriptorSetLayout>(setLayouts.data(), setLayouts.size()),
		});

		if (!result)
		{
			ERROR(LogCategory::App, "ScriptedSceneLayer: failed to create default pipeline");
			return;
		}
		m_defaultPipeline = std::move(result.value());
	}

	void ScriptedSceneLayer::DestroySceneEntities(LayerContext& context)
	{
		auto& world = context.Get<World>();
		for (aether::Entity e: m_sceneCtx.sceneEntities)
		{
			world.Destroy(e);
		}
		m_sceneCtx.sceneEntities.clear();
		m_sceneCtx.loadedModels.clear();

		for (const auto& name: m_sceneCtx.registeredSystems)
		{
			world.UnregisterSystem(name.c_str());
		}
		m_sceneCtx.registeredSystems.clear();
	}

	void ScriptedSceneLayer::DoReload(LayerContext& context)
	{
		INFO(LogCategory::App, "ScriptedSceneLayer: reloading '{}'", m_scriptPath);

		m_scripting->CallOnDetach(m_handle, m_sceneCtx);

		// Frames in flight may still reference the buffers backing the scene
		// entities we're about to destroy. Hot-reload is out-of-band, so a
		// device-wide wait is acceptable.
		context.Get<GpuDevice>().WaitIdle();

		DestroySceneEntities(context);

		scripting::ScriptHandle newHandle = m_scripting->Compile(m_scriptPath);
		if (!newHandle.IsValid())
		{
			WARN(LogCategory::App, "ScriptedSceneLayer: reload failed - keeping old scene");
			// Reattach old scene
			m_scripting->CallOnAttach(m_handle, m_sceneCtx);
			return;
		}

		// Swap to the new script and call on_attach.
		m_scripting->FreeHandle(m_handle);
		m_handle = std::move(newHandle);
		m_scripting->CallOnAttach(m_handle, m_sceneCtx);

		INFO(LogCategory::App, "ScriptedSceneLayer: reload complete.");
	}

	// ── AppLayer overrides ────────────────────────────────────────────────────

	void ScriptedSceneLayer::OnAttach(LayerContext& context)
	{
		m_scripting = context.TryGet<scripting::ScriptingSubsystem>();
		if (!m_scripting)
		{
			ERROR(LogCategory::App, "ScriptedSceneLayer: ScriptingSubsystem not in ServiceContainer");
			return;
		}

		BuildDefaultPipeline(context);

		m_sceneCtx.world = &context.Get<World>();
		m_sceneCtx.assets = &context.Get<AssetManager>();
		m_sceneCtx.cameras = &context.Get<CameraManager>();
		m_sceneCtx.renderer = &context.Get<Renderer>();
		m_sceneCtx.input = &context.Get<Input>();
		m_sceneCtx.systemFactory = &m_systemFactory;
		m_sceneCtx.defaultPipeline = &m_defaultPipeline;
		m_sceneCtx.scriptPath = m_scriptPath;

		m_handle = m_scripting->Compile(m_scriptPath);
		if (!m_handle.IsValid())
		{
			WARN(LogCategory::App, "ScriptedSceneLayer: initial compile failed for '{}'", m_scriptPath);
			return;
		}

		m_scripting->CallOnAttach(m_handle, m_sceneCtx);
	}

	void ScriptedSceneLayer::OnDetach(LayerContext& context)
	{
		if (!m_scripting)
		{
			return;
		}
		m_scripting->CallOnDetach(m_handle, m_sceneCtx);
		DestroySceneEntities(context);
		m_scripting->FreeHandle(m_handle);
	}

	void ScriptedSceneLayer::OnUpdate(LayerContext& context)
	{
		if (!m_scripting || !m_handle.IsValid())
		{
			return;
		}

		m_sceneCtx.deltaTime = static_cast<float>(context.deltaTimeSeconds);

		// Handle hot-reload request (from F5 in DebugLayer).
		if (m_scripting->HasReloadRequest())
		{
			m_scripting->ClearReloadRequest();
			DoReload(context);
			return;
		}

		m_scripting->CallOnUpdate(m_handle, m_sceneCtx);
	}
} // namespace aether::app
