#include "rendering/ModelPreviewService.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/AssetManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/PipelineCache.hpp"
#include "material/TextureRegistry.hpp"
#include "passes/CullPass.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RenderGraphTypes.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/Components.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// World-space bounding sphere of a local sphere under `model` (the
		// WorldRenderer culling transform, replicated for camera fitting).
		glm::vec4 TransformSphere(const glm::vec4& localSphere, const glm::mat4& model)
		{
			const glm::vec3 center = glm::vec3(model * glm::vec4(localSphere.x, localSphere.y, localSphere.z, 1.0f));
			const float maxScale = std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])), glm::length(glm::vec3(model[2]))});
			return glm::vec4(center, localSphere.w * maxScale);
		}

		// Minimal enclosing sphere of two spheres.
		glm::vec4 MergeSpheres(const glm::vec4& a, const glm::vec4& b)
		{
			const glm::vec3 d = glm::vec3(b) - glm::vec3(a);
			const float dist = glm::length(d);
			if (dist + b.w <= a.w)
			{
				return a;
			}
			if (dist + a.w <= b.w)
			{
				return b;
			}
			const float radius = (dist + a.w + b.w) * 0.5f;
			const glm::vec3 center = glm::vec3(a) + (dist > 1e-5f ? d * ((radius - a.w) / dist) : glm::vec3(0.0f));
			return glm::vec4(center, radius);
		}
	} // namespace

	void ModelPreviewService::Initialize(VulkanContext& context, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines, const gpu::Format colorFormat, const gpu::Format depthFormat)
	{
		AE_PROFILE_ZONE();
		(void) context;
		(void) bindless;
		m_colorFormat = colorFormat;
		m_depthFormat = depthFormat;

		m_queue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 2048u, .debugName = "ModelPreview"});
		m_constants.Initialize();

		m_colorHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = colorFormat,
		        .extent = {kSize, kSize},
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "ModelPreview.Color",
		});
		m_depthHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = depthFormat,
		        .extent = {kSize, kSize},
		        .usage = gpu::ImageUsage::DepthStencilAttachment,
		        .aspect = gpu::ImageAspect::Depth,
		        .debugName = "ModelPreview.Depth",
		});
		m_colorLdrHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = gpu::Format::R8G8B8A8Unorm,
		        .extent = {kSize, kSize},
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "ModelPreview.ColorLdr",
		});
		if (m_colorHandle.IsValid())
		{
			m_colorView = gpu::ResourceRegistry::ResolveTexture(m_colorHandle).view;
			gpu::ResourceRegistry::EnsureBindlessSampled(m_colorHandle, gpu::ImageAspect::Color, gpu::ImageLayout::ShaderReadOnly);
			m_colorBindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(m_colorHandle);
		}
		if (m_colorLdrHandle.IsValid())
		{
			m_colorLdrView = gpu::ResourceRegistry::ResolveTexture(m_colorLdrHandle).view;
		}

		m_initialized = m_colorHandle.IsValid() && m_depthHandle.IsValid() && m_colorLdrHandle.IsValid();
	}

	void ModelPreviewService::Shutdown(AssetManager* assets)
	{
		if (assets != nullptr)
		{
			ClearModel(*assets);
		}
		m_queue.DiscardAllPending();
		m_constants.Shutdown();
		for (gpu::TextureHandle* handle: {&m_colorHandle, &m_depthHandle, &m_colorLdrHandle})
		{
			if (handle->IsValid())
			{
				gpu::ResourceRegistry::Destroy(*handle);
				*handle = {};
			}
		}
		m_colorView = nullptr;
		m_colorLdrView = nullptr;
		m_colorBindlessSlot = 0xFFFFFFFFu;
		m_initialized = false;
	}

	void ModelPreviewService::DestroyModelEntities(AssetManager& assets)
	{
		// Release the registry material each entity acquired via AssignMaterial,
		// then drop the entities themselves.
		auto& registry = m_world.GetRegistry();
		std::vector<Entity> entities;
		for (const auto handle: registry.storage<entt::entity>())
		{
			if (registry.valid(handle))
			{
				entities.push_back(World::FromEntt(handle));
			}
		}
		for (const Entity e: entities)
		{
			if (const auto* mc = m_world.TryGet<MaterialComponent>(e))
			{
				assets.GetMaterialRegistry().Release(mc->handle);
			}
			m_world.Destroy(e);
		}
	}

	bool ModelPreviewService::ShowModel(AssetManager& assets, const std::string& path, std::string& outError)
	{
		if (!m_initialized)
		{
			outError = "Model preview renderer is unavailable.";
			return false;
		}
		ClearModel(assets);

		auto loaded = assets.LoadModel(path);
		if (!loaded)
		{
			outError = "Model load failed: " + loaded.error().ToString();
			AE_WARN(LogCategory::Render, "ModelPreview: {} ({})", outError, path);
			return false;
		}
		m_model = std::move(*loaded);
		if (m_model.primitives.empty())
		{
			outError = "The model has no primitives.";
			m_model = LoadedModel{};
			return false;
		}

		glm::vec4 bounds(0.0f, 0.0f, 0.0f, 0.0f);
		bool haveBounds = false;
		for (std::size_t i = 0; i < m_model.primitives.size(); ++i)
		{
			LoadedModelPrimitive& primitive = m_model.primitives[i];

			const Entity e = m_world.Create();
			auto& transform = m_world.Emplace<TransformComponent>(e);
			transform.localToWorld = primitive.localTransform;
			m_world.Emplace<MeshComponent>(e, MeshComponent{.mesh = &primitive.mesh});

			if (primitive.hasMaterial)
			{
				// AssignMaterial acquires registry refs of its own (MaterialComponent
				// + PipelineComponent); the asset-level texture refs LoadModel gave us
				// are released below once every primitive has been assigned.
				MaterialSystem::AssignMaterial(m_world, e, assets.GetMaterialRegistry(), assets.GetPipelineCache(), primitive.material);
			}
			else
			{
				// Vertex-colour fallback via the default two-sided gltf pipeline
				// (mirrors ModelSpawn for material-less primitives).
				MaterialTemplate tmpl{.shaderVfsPath = "shaders://gltf_mesh.spv"};
				tmpl.cullMode = gpu::CullMode::None;
				const GraphicsPipeline* pipeline = assets.GetPipelineCache().Acquire(tmpl);
				m_world.Emplace<PipelineComponent>(e, PipelineComponent{.pipeline = pipeline});
			}

			const glm::vec4 sphere = TransformSphere(primitive.mesh.GetBoundingSphere(), primitive.localTransform);
			bounds = haveBounds ? MergeSpheres(bounds, sphere) : sphere;
			haveBounds = true;
		}

		// The MaterialAssets only existed to hand to AssignMaterial - drop their
		// texture refs now so ClearModel never has to reason about them.
		auto& textures = assets.GetTextureRegistry();
		for (LoadedModelPrimitive& primitive: m_model.primitives)
		{
			if (!primitive.hasMaterial)
			{
				continue;
			}
			for (TextureHandle handle: {primitive.material.albedoTex, primitive.material.normalTex, primitive.material.metallicRoughnessTex, primitive.material.occlusionTex, primitive.material.emissiveTex})
			{
				if (handle.IsValid())
				{
					textures.Release(handle);
				}
			}
			primitive.material = MaterialAsset{};
		}

		m_bounds = glm::vec4(glm::vec3(bounds), std::max(bounds.w, 0.01f));
		m_turntableAngle = glm::radians(30.0f);
		m_hasModel.store(true, std::memory_order_release);
		return true;
	}

	void ModelPreviewService::ClearModel(AssetManager& assets)
	{
		m_hasModel.store(false, std::memory_order_release);
		DestroyModelEntities(assets);
		// The meshes' GPU buffers free through the resource registry's deferred
		// path, so dropping the LoadedModel is safe with frames in flight.
		m_model = LoadedModel{};
		m_bounds = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	void ModelPreviewService::PrepareQueue(const std::uint32_t drawSlot)
	{
		if (!m_initialized)
		{
			return;
		}
		m_queue.SetWriteSlot(drawSlot);
		m_queue.Clear(drawSlot);
		if (!m_hasModel.load(std::memory_order_acquire))
		{
			return;
		}

		// Turntable: slow orbit around the merged bounds, slightly above centre.
		m_turntableAngle += glm::radians(0.55f);
		const glm::vec3 center = glm::vec3(m_bounds);
		const float radius = m_bounds.w;
		const float distance = radius * 2.4f;
		const glm::vec3 eye = center + glm::vec3(std::cos(m_turntableAngle) * distance, distance * 0.45f, std::sin(m_turntableAngle) * distance);
		const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
		glm::mat4 proj = glm::perspective(glm::radians(40.0f), 1.0f, std::max(0.02f, radius * 0.05f), std::max(10.0f, radius * 20.0f));
		proj[1][1] *= -1.0f; // Vulkan clip-space Y flip (matches the engine's camera projections)

		{
			std::lock_guard<std::mutex> lock(m_cameraMutex);
			m_camView = view;
			m_camProj = proj;
			m_camPos = eye;
		}

		WorldRenderer::Flush(m_world, m_queue, /*shadowPass*/ false);
	}

	void ModelPreviewService::BuildFrameConstants(const FrameConstants& mainFc, const std::uint32_t frameIdx)
	{
		if (!m_initialized)
		{
			return;
		}
		FrameConstants fc = mainFc;
		{
			std::lock_guard<std::mutex> lock(m_cameraMutex);
			fc.view = m_camView;
			fc.proj = m_camProj;
			fc.viewProj = m_camProj * m_camView;
			fc.cameraWorldPos = glm::vec4(m_camPos, 1.0f);
		}
		// The live scene's tile lists / shadow maps have no meaning for this
		// isolated model: keep sun + ambient shading, drop everything scene-local.
		fc.tiledLightGridInfo = glm::uvec4(0u);
		fc.tiledLightBufferOffsets = glm::uvec4(0u);
		fc.shadowParams.z = 0.0f; // shadow strength: never sample the scene's cascades
		fc.shadowLightCount = 0;
		fc.RefreshDerived();
		m_constants.Write(frameIdx, fc);
	}

	void ModelPreviewService::RegisterImages(RenderGraph& graph)
	{
		if (!m_initialized)
		{
			return;
		}
		m_color = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_colorHandle), m_colorView, gpu::ImageAspect::Color);
		m_colorLdr = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_colorLdrHandle), m_colorLdrView, gpu::ImageAspect::Color);
		const auto depthTex = gpu::ResourceRegistry::ResolveTexture(m_depthHandle);
		m_depth = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_depthHandle), depthTex.view, gpu::ImageAspect::Depth);
	}

	void ModelPreviewService::RegisterComputePasses(RenderGraph& graph, CullPass& cullPass)
	{
		if (!m_initialized)
		{
			return;
		}
		RegisterImages(graph);
		m_draws = graph.CreatePreparedDrawList("ModelPreviewDraws");

		graph.AddQueuePreparePass({
		             .name = "$ModelPreviewCull",
		             .produces = m_draws,
		             .sideEffectReason = "prepares model-preview draw queue",
		     })
		        .ExecuteCompute(
		                [this, &cullPass](PassContext& ctx)
		                {
			                // Always dispatch so the slot is consumed each frame (empty
			                // queue => 0 draws when no model is staged).
			                m_queue.PrepareAndDispatch(ctx.recorder, m_constants.GetDeviceAddress(ctx.frameSlot), cullPass.GetSinglePipeline(), ctx.frameSlot);
		                })
		        .OnDebugDisabled([this](PassContext& ctx) { m_queue.DiscardPending(ctx.frameSlot); });
	}

	void ModelPreviewService::RegisterGraphicsPasses(RenderGraph& graph, BindlessManager& bindless, const PostProcessStack& postProcess)
	{
		if (!m_initialized)
		{
			return;
		}
		auto pass = graph.AddDrawQueuePass({
		        .name = "$ModelPreviewForward",
		        .color = m_color,
		        .depth = m_depth,
		        .draws = m_draws,
		        .extent = {kSize, kSize},
		        .colorLoadOp = gpu::LoadOp::Clear,
		        .depthLoadOp = gpu::LoadOp::Clear,
		});
		pass.Execute(
		        [this, &bindless](PassContext& ctx)
		        {
			        if (!m_hasModel.load(std::memory_order_relaxed))
			        {
				        return; // colour stays cleared
			        }
			        // No scene lighting addresses: the fc disabled tiles/shadows, so
			        // the shader shades sun + ambient only.
			        const DrawContracts::LightingAddresses lightingAddr{};
			        bindless.CmdBindHeaps(ctx.recorder);
			        m_queue.FlushDrawWithFrameAddr(ctx.recorder, ctx.frameSlot, &lightingAddr, m_constants.GetDeviceAddress(ctx.frameSlot), nullptr, 0, nullptr);
		        });

		// Same tonemap treatment as the camera preview so thumbnails match the
		// main view's exposure/operator.
		graph
		        .AddFullscreenPass({
		                .name = "$ModelPreviewTonemap",
		                .color = m_colorLdr,
		                .extent = {kSize, kSize},
		                .loadOp = gpu::LoadOp::DontCare,
		        })
		        .ReadTexture(m_color)
		        .Execute(
		                [this, &bindless, &postProcess](PassContext& ctx)
		                {
			                gpu::CommandList& cmd = ctx.recorder;
			                bindless.CmdBindHeaps(cmd);
			                cmd.BindPipeline(postProcess.GetTonemapPipeline());
			                struct
			                {
				                std::uint32_t hdrSlot;
				                std::uint32_t mode;
				                float exposure;
				                std::uint32_t debugCompare;
				                std::uint32_t debugModeCount;
				                std::int32_t inspectX;
				                std::int32_t inspectY;
				                std::uint32_t screenWidth;
				                std::uint32_t screenHeight;
			                } push;
			                push.hdrSlot = m_colorBindlessSlot;
			                push.mode = static_cast<std::uint32_t>(postProcess.GetTonemapMode());
			                push.exposure = postProcess.GetExposure();
			                push.debugCompare = 0u;
			                push.debugModeCount = 0u;
			                push.inspectX = -1;
			                push.inspectY = -1;
			                push.screenWidth = kSize;
			                push.screenHeight = kSize;
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                cmd.Draw(3, 1, 0, 0);
		                });

		graph.AddPass("$ModelPreviewReady").ReadTexture(m_colorLdr).Execute([](PassContext&) {});
	}
} // namespace aether
