#include "rendering/LightingManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/common.hpp>
#include <vector>

#include "gpu/GpuDevice.hpp"
#include "gpu/GpuDeviceFactory.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "io/FileSystem.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{
	void LightingManager::Initialize(GpuDevice& device, const VulkanContext& context)
	{
		AE_PROFILE_ZONE();
		m_device = &device;
		m_context = &context;
		m_renderer = nullptr;

		m_views[kMainLightView].registered = true;
		m_views[kMainLightView].debugName = "Main";

		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			EnsureLightsBuffer(i, 1);
		}
	}

	void LightingManager::LinkRenderer(const Renderer& renderer)
	{
		m_renderer = &renderer;
	}

	void LightingManager::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (!m_context)
		{
			return;
		}

		for (auto& frame: m_lightBuffers)
		{
			if (frame.lightsHandle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(frame.lightsHandle);
				frame.lightsHandle = {};
			}
			for (auto& stale: frame.staleBuffers)
			{
				if (stale.IsValid())
				{
					gpu::ResourceRegistry::Destroy(stale);
				}
			}
			frame.staleBuffers.clear();
			frame.lightsMapped = nullptr;
			frame.lightsBuffer = nullptr;
			frame.lightsDeviceAddr = 0;
			frame.lightsSize = 0;
			frame.lightsCapacity = 0;
			frame.lightCount = 0;
		}

		for (auto& view: m_views)
		{
			DestroyViewBuffers(view);
			view.registered = false;
			view.debugName.clear();
		}

		if (m_cullPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_cullPipelineHandle);
			m_cullPipelineHandle = {};
		}

		m_context = nullptr;
		m_renderer = nullptr;
		m_device = nullptr;
	}

	LightViewId LightingManager::RegisterView(std::string debugName)
	{
		for (LightViewId id = 1; id < kMaxLightViews; ++id)
		{
			if (!m_views[id].registered)
			{
				m_views[id].registered = true;
				m_views[id].debugName = std::move(debugName);
				return id;
			}
		}
		AE_WARN(LogCategory::Render, "LightingManager: light-view pool exhausted ({} views); '{}' will shade without local lights.", kMaxLightViews, debugName);
		return kInvalidLightView;
	}

	void LightingManager::UnregisterView(const LightViewId viewId)
	{
		if (viewId == kMainLightView || viewId >= kMaxLightViews || !m_views[viewId].registered)
		{
			return;
		}
		DestroyViewBuffers(m_views[viewId]);
		m_views[viewId].registered = false;
		m_views[viewId].debugName.clear();
	}

	void LightingManager::BuildLightList(std::vector<GpuLight>& outLights, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights)
	{
		outLights.reserve(outLights.size() + pointLights.size() + spotLights.size());

		for (const auto& src: pointLights)
		{
			outLights.push_back(GpuLight{
			        .positionRadius = glm::vec4(src.position, src.radius),
			        .colorIntensity = glm::vec4(src.color, src.intensity),
			        .directionType = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
			        .params = glm::vec4(0.0f),
			        .shadowIndex = glm::vec4(-1.0f, 1.0f, 0.0f, 0.0f),
			});
		}
		for (const auto& src: spotLights)
		{
			outLights.push_back(GpuLight{
			        .positionRadius = glm::vec4(src.position, src.radius),
			        .colorIntensity = glm::vec4(src.color, src.intensity),
			        .directionType = glm::vec4(glm::normalize(src.direction), 1.0f),
			        .params = glm::vec4(std::cos(src.innerAngleRad), std::cos(src.outerAngleRad), 0.0f, 0.0f),
			        .shadowIndex = glm::vec4(-1.0f, 1.0f, 0.0f, 0.0f),
			});
		}
	}

	void LightingManager::EnsureLightsBuffer(const std::uint32_t frameSlot, const std::size_t lightCount) const
	{
		AE_PROFILE_ZONE();
		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		auto& frame = m_lightBuffers[slot];

		for (auto& stale: frame.staleBuffers)
		{
			if (stale.IsValid())
			{
				gpu::ResourceRegistry::Destroy(stale);
			}
		}
		frame.staleBuffers.clear();

		const std::size_t safeRequired = std::max<std::size_t>(lightCount, 1u);
		if (frame.lightsHandle.IsValid() && frame.lightsCapacity >= safeRequired)
		{
			return;
		}

		frame.lightsCapacity = std::max(safeRequired, frame.lightsCapacity * 2u);

		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(sizeof(GpuLight) * frame.lightsCapacity),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::Auto,
		        .debugName = "LightingManager.Lights",
		};
		const auto newHandle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!newHandle.IsValid())
		{
			Throw(AetherError::Engine("LightingManager: CreateMappedBuffer failed"));
		}

		if (frame.lightsHandle.IsValid())
		{
			frame.staleBuffers.push_back(frame.lightsHandle);
		}
		frame.lightsHandle = newHandle;

		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.lightsHandle);
		frame.lightsMapped = view.mappedPtr;
		frame.lightsSize = view.size;
		frame.lightsBuffer = static_cast<gpu::Buffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(frame.lightsHandle));
		frame.lightsDeviceAddr = gpu::ResourceRegistry::ResolveBuffer(frame.lightsHandle).deviceAddress;
		AE_ASSERT_ALWAYS(frame.lightsDeviceAddr != 0, "LightingManager lights buffer requires a valid shader device address.");
	}

	void LightingManager::EnsureViewBuffers(View& view, const std::uint32_t slot, const std::size_t tileCount, const std::size_t indexCount) const
	{
		AE_PROFILE_ZONE();
		auto& vs = view.slots[slot];
		auto& frame = m_lightBuffers[slot];

		auto ensure = [&](gpu::BufferHandle& handle, gpu::Buffer& buffer, gpu::DeviceAddress& addr, std::size_t& capacity, const std::size_t required, const std::size_t stride, const char* debugName)
		{
			const std::size_t safeRequired = std::max<std::size_t>(required, 1u);
			if (handle.IsValid() && capacity >= safeRequired)
			{
				return;
			}
			capacity = std::max(safeRequired, capacity * 2u);

			// forward passes - the CPU never touches them.
			const auto newHandle = gpu::ResourceRegistry::CreateBuffer({
			        .size = static_cast<gpu::DeviceSize>(stride * capacity),
			        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
			        .debugName = debugName,
			});
			if (!newHandle.IsValid())
			{
				Throw(AetherError::Engine("LightingManager: view tile buffer creation failed"));
			}
			if (handle.IsValid())
			{
				frame.staleBuffers.push_back(handle);
			}
			handle = newHandle;
			buffer = static_cast<gpu::Buffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(handle));
			addr = gpu::ResourceRegistry::ResolveBuffer(handle).deviceAddress;
			AE_ASSERT_ALWAYS(addr != 0, "LightingManager view tile buffer requires a valid shader device address.");
		};

		ensure(vs.headersHandle, vs.headersBuffer, vs.headersAddr, vs.headersCapacity, tileCount, sizeof(TileHeader), "LightingManager.TileHeaders");
		ensure(vs.indicesHandle, vs.indicesBuffer, vs.indicesAddr, vs.indicesCapacity, indexCount, sizeof(std::uint32_t), "LightingManager.TileIndices");
	}

	void LightingManager::DestroyViewBuffers(View& view)
	{
		for (auto& vs: view.slots)
		{
			if (vs.headersHandle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(vs.headersHandle);
			}
			if (vs.indicesHandle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(vs.indicesHandle);
			}
			vs = ViewSlot{};
		}
	}

	void LightingManager::StageViewDispatch(View& view, const std::uint32_t slot, const glm::mat4& viewMat, const glm::mat4& proj, const float nearPlane, const gpu::Extent2D extent, FrameConstants& fc) const
	{
		auto& frame = m_lightBuffers[slot];
		auto& vs = view.slots[slot];

		const std::uint32_t tilesX = (extent.width + kTileSizePx - 1u) / kTileSizePx;
		const std::uint32_t tilesY = (extent.height + kTileSizePx - 1u) / kTileSizePx;
		const std::size_t tileCount = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);

		EnsureViewBuffers(view, slot, tileCount, tileCount * static_cast<std::size_t>(m_maxLightsPerTile));

		vs.push.lightDataAddr = frame.lightsDeviceAddr;
		vs.push.tileHeadersAddr = vs.headersAddr;
		vs.push.tileLightIndicesAddr = vs.indicesAddr;
		vs.push.view = viewMat;
		vs.push.params0 = glm::vec4(nearPlane, 1.0f / proj[0][0], 1.0f / proj[1][1], 0.0f);
		vs.push.params1 = glm::uvec4(kTileSizePx, tilesX, tilesY, frame.lightCount);
		vs.push.params2 = glm::uvec4(m_maxLightsPerTile, extent.width, extent.height, 0u);
		vs.tilesX = tilesX;
		vs.tilesY = tilesY;
		vs.ready = true;

		fc.tiledLightGridInfo = glm::uvec4(kTileSizePx, tilesX, tilesY, frame.lightCount);
		fc.tiledLightBufferOffsets = glm::uvec4(0u, 0u, 0u, m_maxLightsPerTile);
	}

	void LightingManager::EnsureComputePipeline() const
	{
		if (m_cullPipelineHandle.IsValid())
		{
			return;
		}

		auto* const device = m_context->GetDevice().device;
		m_cullPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(device,
		        gpu::ComputePipelineDesc{
		                .shaderVfsPath = "shaders://tiled_light_cull.spv",
		                .shaderEntry = "main",
		                .debugName = "LightCull.BinLights",
		        });
		if (!m_cullPipelineHandle.IsValid())
		{
			Throw(AetherError::Vulkan(0, "LightingManager: failed to create binLights compute pipeline."));
		}
	}

	DrawContracts::LightingAddresses LightingManager::GetLightingAddresses(const std::uint32_t frameSlot) const
	{
		return GetLightingAddresses(kMainLightView, frameSlot);
	}

	DrawContracts::LightingAddresses LightingManager::GetLightingAddresses(const LightViewId viewId, const std::uint32_t frameSlot) const
	{
		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		if (viewId >= kMaxLightViews || !m_views[viewId].registered)
		{
			return DrawContracts::LightingAddresses{};
		}
		const auto& vs = m_views[viewId].slots[slot];
		return DrawContracts::LightingAddresses{
		        .lightDataAddr = m_lightBuffers[slot].lightsDeviceAddr,
		        .tileHeadersAddr = vs.headersAddr,
		        .tileLightIndicesAddr = vs.indicesAddr,
		};
	}

	void LightingManager::ApplyShadowIndices(const std::uint32_t frameSlot, const std::span<const glm::vec2> shadowIndices)
	{
		AE_PROFILE_ZONE();
		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		auto& frame = m_lightBuffers[slot];
		if (!frame.lightsHandle.IsValid() || frame.lightsMapped == nullptr || shadowIndices.empty())
		{
			return;
		}

		const std::size_t lightCount = frame.lightCount;
		const std::size_t applyCount = std::min(lightCount, shadowIndices.size());
		auto* mapped = static_cast<GpuLight*>(frame.lightsMapped);
		for (std::size_t i = 0; i < applyCount; ++i)
		{
			mapped[i].shadowIndex.x = shadowIndices[i].x;
			mapped[i].shadowIndex.y = shadowIndices[i].y;
		}
		gpu::ResourceRegistry::FlushMappedBuffer(frame.lightsHandle, 0, static_cast<gpu::DeviceSize>(applyCount) * sizeof(GpuLight));
	}

	void LightingManager::DisableForView(FrameConstants& fc)
	{
		fc.tiledLightGridInfo = glm::uvec4(0u);
		fc.tiledLightBufferOffsets = glm::uvec4(0u);
	}

	void LightingManager::RegisterPasses(RenderGraph& graph)
	{
		EnsureComputePipeline();

		m_rgLights = graph.RegisterBuffer(nullptr);
		m_rgTileHeaders = graph.RegisterBuffer(nullptr);
		m_rgTileIndices = graph.RegisterBuffer(nullptr);
		(void) graph.GetBlackboard().DeclareGraphProduct<LightBuffersProduct>(std::string{kFrameProductLightBuffers},
		        LightBuffersProduct{
		                .lights = m_rgLights,
		                .tileHeaders = m_rgTileHeaders,
		                .tileIndices = m_rgTileIndices,
		        });

		const auto cullResolved = gpu::ResourceRegistry::ResolvePipeline(m_cullPipelineHandle);

		graph.AddComputeBufferPass({
		                                   .name = "$Lighting.BinLights",
		                                   .reads = {m_rgLights},
		                                   .readWrites = {m_rgTileHeaders, m_rgTileIndices},
		                                   .consumes = {RenderGraph::Product<MainViewProduct>(kFrameProductMainView)},
		                                   .produces = {RenderGraph::Product<LightBuffersProduct>(kFrameProductLightBuffers)},
		                           })
		        .ExecuteCompute(
		                [this, cullPipeline = cullResolved.state](PassContext& ctx)
		                {
			                const std::uint32_t slot = ctx.frameSlot;
			                if (!m_lightDataReady[slot])
			                {
				                return;
			                }
			                gpu::CommandList cmd = ctx.recorder.View();
			                bool bound = false;
			                for (auto& view: m_views)
			                {
				                auto& vs = view.slots[slot];
				                if (!view.registered || !vs.ready)
				                {
					                continue;
				                }
				                if (!bound)
				                {
					                cmd.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);
					                cmd.BindComputePipeline(cullPipeline);
					                bound = true;
				                }
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(vs.push));
				                cmd.Dispatch(vs.tilesX, vs.tilesY, 1);
				                vs.ready = false;
			                }
			                if (bound)
			                {
				                cmd.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader,
				                        gpu::AccessFlags::ShaderStorageWrite,
				                        gpu::PipelineStage::FragmentShader | gpu::PipelineStage::ComputeShader,
				                        gpu::AccessFlags::ShaderRead | gpu::AccessFlags::ShaderStorageRead);
			                }
		                });
	}

	bool LightingManager::PrepareForRenderGraph(
	        const std::uint32_t frameSlot, const Camera& camera, const gpu::Extent2D extent, FrameConstants& fc, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights)
	{
		const float aspect = extent.height != 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0f;
		return PrepareForRenderGraph(frameSlot, camera.GetViewMatrix(), camera.GetProjectionMatrix(aspect), camera.GetNearPlane(), extent, fc, pointLights, spotLights);
	}

	bool LightingManager::PrepareForRenderGraph(const std::uint32_t frameSlot,
	        const glm::mat4& view,
	        const glm::mat4& proj,
	        const float nearPlane,
	        const gpu::Extent2D extent,
	        FrameConstants& fc,
	        const std::span<const Renderer::PointLight> pointLights,
	        const std::span<const Renderer::SpotLight> spotLights)
	{
		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		if (extent.width == 0 || extent.height == 0)
		{
			DisableForView(fc);
			m_lightDataReady[slot] = false;
			return false;
		}

		std::vector<GpuLight> lights;
		BuildLightList(lights, pointLights, spotLights);

		if (lights.empty())
		{
			DisableForView(fc);
			m_lightDataReady[slot] = false;
			m_lightBuffers[slot].lightCount = 0;
			return false;
		}

		EnsureLightsBuffer(slot, lights.size());
		auto& frame = m_lightBuffers[slot];
		std::memcpy(frame.lightsMapped, lights.data(), lights.size() * sizeof(GpuLight));
		gpu::ResourceRegistry::FlushMappedBuffer(frame.lightsHandle, 0, static_cast<gpu::DeviceSize>(lights.size()) * sizeof(GpuLight));
		frame.lightCount = static_cast<std::uint32_t>(lights.size());

		EnsureComputePipeline();

		StageViewDispatch(m_views[kMainLightView], slot, view, proj, nearPlane, extent, fc);
		m_lightDataReady[slot] = true;
		return true;
	}

	bool LightingManager::PrepareView(const LightViewId viewId, const std::uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj, const float nearPlane, const gpu::Extent2D extent, FrameConstants& fc)
	{
		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		if (viewId == kMainLightView || viewId >= kMaxLightViews || !m_views[viewId].registered || extent.width == 0 || extent.height == 0 || !m_lightDataReady[slot] || m_lightBuffers[slot].lightCount == 0)
		{
			DisableForView(fc);
			return false;
		}

		StageViewDispatch(m_views[viewId], slot, view, proj, nearPlane, extent, fc);
		return true;
	}

	void LightingManager::RecordBinLights(const LightViewId viewId, const std::uint32_t frameSlot, gpu::CommandList cmd)
	{
		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		if (viewId >= kMaxLightViews || !m_views[viewId].registered)
		{
			return;
		}
		auto& vs = m_views[viewId].slots[slot];
		if (!vs.ready)
		{
			return;
		}
		vs.ready = false;

		const auto cullResolved = gpu::ResourceRegistry::ResolvePipeline(m_cullPipelineHandle);

		cmd.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);
		cmd.BindComputePipeline(cullResolved.state);
		cmd.PushDataRaw(0, gpu::AsPushConstantBytes(vs.push));
		cmd.Dispatch(vs.tilesX, vs.tilesY, 1);
		cmd.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::FragmentShader | gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderRead | gpu::AccessFlags::ShaderStorageRead);
	}

	void LightingManager::UpdateBufferHandles(RenderGraph& graph, const std::uint32_t frameSlot) const
	{
		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		const auto& mainSlot = m_views[kMainLightView].slots[slot];
		graph.UpdateExternalBuffer(m_rgLights, static_cast<void*>(m_lightBuffers[slot].lightsBuffer));
		graph.UpdateExternalBuffer(m_rgTileHeaders, static_cast<void*>(mainSlot.headersBuffer));
		graph.UpdateExternalBuffer(m_rgTileIndices, static_cast<void*>(mainSlot.indicesBuffer));
	}
} // namespace aether
