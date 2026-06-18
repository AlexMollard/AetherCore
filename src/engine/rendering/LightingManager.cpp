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

		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			EnsureBuffers(i, 1, 1, 1);
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

		for (auto& frame: m_buffers)
		{
			auto destroyBuf = [](gpu::BufferHandle& h)
			{
				if (h.IsValid())
				{
					gpu::ResourceRegistry::Destroy(h);
					h = {};
				}
			};
			destroyBuf(frame.lightsHandle);
			destroyBuf(frame.tileHeadersHandle);
			destroyBuf(frame.tileIndicesHandle);
			for (auto& stale: frame.staleBuffers)
			{
				if (stale.IsValid())
				{
					gpu::ResourceRegistry::Destroy(stale);
				}
			}
			frame.staleBuffers.clear();
			frame.lightsMapped = nullptr;
			frame.tileHeadersMapped = nullptr;
			frame.tileIndicesMapped = nullptr;
			frame.lightsBuffer = nullptr;
			frame.tileHeadersBuffer = nullptr;
			frame.tileIndicesBuffer = nullptr;
			frame.lightsSize = 0;
			frame.tileHeadersSize = 0;
			frame.tileIndicesSize = 0;
			frame.lightsCapacity = 0;
			frame.headersCapacity = 0;
			frame.indicesCapacity = 0;
			frame.lightsDeviceAddr = 0;
			frame.tileHeadersDeviceAddr = 0;
			frame.tileIndicesDeviceAddr = 0;
		}

		if (m_initPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_initPipelineHandle);
			m_initPipelineHandle = {};
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

	void LightingManager::UpdateForView(const std::uint32_t frameSlot,
	        const Camera& camera,
	        const gpu::Extent2D extent,
	        FrameConstants& fc,
	        const bool enableBinningForView,
	        const std::span<const Renderer::PointLight> pointLights,
	        const std::span<const Renderer::SpotLight> spotLights) const
	{
		AE_PROFILE_ZONE();
		if (!enableBinningForView || extent.width == 0 || extent.height == 0)
		{
			DisableForView(fc);
			return;
		}

		UpdateForViewCpu(frameSlot, camera, extent, fc, pointLights, spotLights);
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

	void LightingManager::EmitAcquireBarriers(const std::uint32_t /*frameSlot*/, gpu::CommandList& /*graphicsCmd*/, const std::uint32_t /*srcFamily*/, const std::uint32_t /*dstFamily*/) const
	{
		// maintenance9 eliminates queue family ownership transfers entirely.
	}

	void LightingManager::UpdateForViewCpu(
	        const std::uint32_t frameSlot, const Camera& camera, const gpu::Extent2D extent, FrameConstants& fc, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights) const
	{
		AE_PROFILE_ZONE();
		std::vector<GpuLight> lights;
		BuildLightList(lights, pointLights, spotLights);

		const std::uint32_t tilesX = (extent.width + kTileSizePx - 1u) / kTileSizePx;
		const std::uint32_t tilesY = (extent.height + kTileSizePx - 1u) / kTileSizePx;
		const std::size_t tileCount = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);
		std::vector<TileHeader> headers(tileCount);
		std::vector<std::uint32_t> counts(tileCount, 0u);

		struct ScreenBounds
		{
			int minTx = 0;
			int maxTx = -1;
			int minTy = 0;
			int maxTy = -1;
			bool visible = false;
		};

		std::vector<ScreenBounds> bounds(lights.size());

		const glm::mat4 view = camera.GetViewMatrix();
		const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
		const glm::mat4 proj = camera.GetProjectionMatrix(aspect);
		const glm::mat4 viewProj = proj * view;
		const float pixelScaleY = 0.5f * static_cast<float>(extent.height) * std::abs(proj[1][1]);
		const float nearClip = camera.GetNearPlane();

		auto computeLightBounds = [&](const GpuLight& light, ScreenBounds& out)
		{
			const glm::vec4 viewPos4 = view * glm::vec4(light.positionRadius.x, light.positionRadius.y, light.positionRadius.z, 1.0f);
			const float depth = -viewPos4.z;
			if (depth < nearClip)
			{
				out.visible = false;
				return;
			}

			const glm::vec4 clip = viewProj * glm::vec4(light.positionRadius.x, light.positionRadius.y, light.positionRadius.z, 1.0f);
			if (std::abs(clip.w) <= 1e-6f)
			{
				out.visible = false;
				return;
			}

			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			const float screenX = (ndc.x * 0.5f + 0.5f) * static_cast<float>(extent.width);
			const float screenY = (ndc.y * 0.5f + 0.5f) * static_cast<float>(extent.height);
			const float r = light.positionRadius.w;
			const float effectiveDepth = std::max(std::sqrt(std::max(depth * depth - r * r, 0.0f)), nearClip);
			const float radiusPx = r * pixelScaleY / effectiveDepth;
			if (radiusPx <= 0.5f)
			{
				out.visible = false;
				return;
			}

			const float minX = screenX - radiusPx;
			const float maxX = screenX + radiusPx;
			const float minY = screenY - radiusPx;
			const float maxY = screenY + radiusPx;

			if (maxX < 0.0f || maxY < 0.0f || minX >= static_cast<float>(extent.width) || minY >= static_cast<float>(extent.height))
			{
				out.visible = false;
				return;
			}

			out.minTx = static_cast<int>(glm::clamp(std::floor(minX / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesX - 1u)));
			out.maxTx = static_cast<int>(glm::clamp(std::floor(maxX / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesX - 1u)));
			out.minTy = static_cast<int>(glm::clamp(std::floor(minY / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesY - 1u)));
			out.maxTy = static_cast<int>(glm::clamp(std::floor(maxY / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesY - 1u)));
			out.visible = true;
		};

		for (std::size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
		{
			computeLightBounds(lights[lightIndex], bounds[lightIndex]);
			if (!bounds[lightIndex].visible)
			{
				continue;
			}
			for (int ty = bounds[lightIndex].minTy; ty <= bounds[lightIndex].maxTy; ++ty)
			{
				for (int tx = bounds[lightIndex].minTx; tx <= bounds[lightIndex].maxTx; ++tx)
				{
					const std::size_t tile = static_cast<std::size_t>(ty) * tilesX + static_cast<std::size_t>(tx);
					++counts[tile];
				}
			}
		}

		std::uint32_t totalIndices = 0;
		for (std::size_t tile = 0; tile < tileCount; ++tile)
		{
			headers[tile].offset = totalIndices;
			headers[tile].count = counts[tile];
			totalIndices += counts[tile];
		}

		std::vector<std::uint32_t> indices(totalIndices);
		std::vector<std::uint32_t> cursors(tileCount, 0u);
		for (std::size_t tile = 0; tile < tileCount; ++tile)
		{
			cursors[tile] = headers[tile].offset;
		}

		for (std::size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
		{
			if (!bounds[lightIndex].visible)
			{
				continue;
			}
			for (int ty = bounds[lightIndex].minTy; ty <= bounds[lightIndex].maxTy; ++ty)
			{
				for (int tx = bounds[lightIndex].minTx; tx <= bounds[lightIndex].maxTx; ++tx)
				{
					const std::size_t tile = static_cast<std::size_t>(ty) * tilesX + static_cast<std::size_t>(tx);
					indices[cursors[tile]++] = static_cast<std::uint32_t>(lightIndex);
				}
			}
		}

		EnsureBuffers(frameSlot, lights.size(), headers.size(), indices.size());
		auto& frame = m_buffers[frameSlot];
		if (!lights.empty())
		{
			std::memcpy(frame.lightsMapped, lights.data(), lights.size() * sizeof(GpuLight));
		}
		if (!headers.empty())
		{
			std::memcpy(frame.tileHeadersMapped, headers.data(), headers.size() * sizeof(TileHeader));
		}
		if (!indices.empty())
		{
			std::memcpy(frame.tileIndicesMapped, indices.data(), indices.size() * sizeof(std::uint32_t));
		}
		gpu::ResourceRegistry::FlushMappedBuffer(frame.lightsHandle, 0, static_cast<gpu::DeviceSize>(lights.size()) * sizeof(GpuLight));
		gpu::ResourceRegistry::FlushMappedBuffer(frame.tileHeadersHandle, 0, static_cast<gpu::DeviceSize>(headers.size()) * sizeof(TileHeader));
		gpu::ResourceRegistry::FlushMappedBuffer(frame.tileIndicesHandle, 0, static_cast<gpu::DeviceSize>(indices.size()) * sizeof(std::uint32_t));

		fc.tiledLightGridInfo = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		fc.tiledLightBufferOffsets = glm::uvec4(0u, 0u, 0u, m_maxLightsPerTile);
	}

	void LightingManager::EnsureBuffers(const std::uint32_t frameSlot, const std::size_t lightCount, const std::size_t tileCount, const std::size_t indexCount) const
	{
		AE_PROFILE_ZONE();
		auto& frame = m_buffers[frameSlot];

		// Retire stale buffers from kMaxFramesInFlight frames ago - this slot is
		// guaranteed to have completed all GPU work referencing them.
		for (auto& stale: frame.staleBuffers)
		{
			if (stale.IsValid())
			{
				gpu::ResourceRegistry::Destroy(stale);
			}
		}
		frame.staleBuffers.clear();

		auto ensureBuffer = [&](gpu::BufferHandle& handle, void*& mapped, gpu::Buffer& buffer, gpu::DeviceSize& size, std::size_t& capacity, const std::size_t required, const std::size_t stride)
		{
			const std::size_t safeRequired = std::max<std::size_t>(required, 1u);
			if (handle.IsValid() && capacity >= safeRequired)
			{
				return;
			}

			capacity = std::max(safeRequired, capacity * 2u);
			if (capacity == 0)
			{
				capacity = safeRequired;
			}

			const gpu::MappedBufferDesc desc{
			        .size = static_cast<gpu::DeviceSize>(stride * capacity),
			        .usage = gpu::BufferUsage::Storage,
			        .memoryUsage = gpu::MappedMemoryUsage::Auto,
			        .debugName = "LightingManager.FrameBuffer",
			};
			const auto newHandle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!newHandle.IsValid())
			{
				Throw(AetherError::Engine("LightingManager: CreateMappedBuffer failed"));
			}

			if (handle.IsValid())
			{
				frame.staleBuffers.push_back(handle);
			}
			handle = newHandle;

			const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(handle);
			mapped = view.mappedPtr;
			size = view.size;
			buffer = static_cast<gpu::Buffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(handle));
		};

		ensureBuffer(frame.lightsHandle, frame.lightsMapped, frame.lightsBuffer, frame.lightsSize, frame.lightsCapacity, lightCount, sizeof(GpuLight));
		ensureBuffer(frame.tileHeadersHandle, frame.tileHeadersMapped, frame.tileHeadersBuffer, frame.tileHeadersSize, frame.headersCapacity, tileCount, sizeof(TileHeader));
		ensureBuffer(frame.tileIndicesHandle, frame.tileIndicesMapped, frame.tileIndicesBuffer, frame.tileIndicesSize, frame.indicesCapacity, indexCount, sizeof(std::uint32_t));

		frame.lightsDeviceAddr = gpu::ResourceRegistry::ResolveBuffer(frame.lightsHandle).deviceAddress;
		frame.tileHeadersDeviceAddr = gpu::ResourceRegistry::ResolveBuffer(frame.tileHeadersHandle).deviceAddress;
		frame.tileIndicesDeviceAddr = gpu::ResourceRegistry::ResolveBuffer(frame.tileIndicesHandle).deviceAddress;
	}

	void LightingManager::EnsureComputePipeline() const
	{
		AE_PROFILE_ZONE();
		if (m_initPipelineHandle.IsValid() && m_cullPipelineHandle.IsValid())
		{
			return;
		}

		auto device = static_cast<gpu::Device>(m_context->GetDevice().device);
		auto pipelineCache = static_cast<gpu::PipelineCache>(m_context->GetPipelineCache());

		if (!m_initPipelineHandle.IsValid())
		{
			m_initPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(device,
			        pipelineCache,
			        gpu::ComputePipelineDesc{
			                .shaderVfsPath = "shaders://tiled_light_cull.spv",
			                .shaderEntry = "initTiles",
			                .debugName = "LightCull.InitTiles",
			        });
			if (!m_initPipelineHandle.IsValid())
			{
				Throw(AetherError::Vulkan(0, "LightingManager: failed to create initTiles compute pipeline."));
			}
		}

		if (!m_cullPipelineHandle.IsValid())
		{
			m_cullPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(device,
			        pipelineCache,
			        gpu::ComputePipelineDesc{
			                .shaderVfsPath = "shaders://tiled_light_cull.spv",
			                .shaderEntry = "binLights",
			                .debugName = "LightCull.BinLights",
			        });
			if (!m_cullPipelineHandle.IsValid())
			{
				Throw(AetherError::Vulkan(0, "LightingManager: failed to create binLights compute pipeline."));
			}
		}
	}

	DrawContracts::LightingAddresses LightingManager::GetLightingAddresses(std::uint32_t frameSlot) const
	{
		auto& frame = m_buffers[frameSlot];
		return DrawContracts::LightingAddresses{
		        .lightDataAddr = frame.lightsDeviceAddr,
		        .tileHeadersAddr = frame.tileHeadersDeviceAddr,
		        .tileLightIndicesAddr = frame.tileIndicesDeviceAddr,
		};
	}

	void LightingManager::ApplyShadowIndices(const std::uint32_t frameSlot, const std::span<const glm::vec2> shadowIndices)
	{
		AE_PROFILE_ZONE();
		auto& frame = m_buffers[frameSlot];
		if (!frame.lightsHandle.IsValid() || shadowIndices.empty())
		{
			return;
		}

		const std::size_t lightCount = frame.lightsSize / sizeof(GpuLight);
		const std::size_t applyCount = std::min(lightCount, shadowIndices.size());
		auto mapped = static_cast<GpuLight*>(frame.lightsMapped);
		for (std::size_t i = 0; i < applyCount; ++i)
		{
			mapped[i].shadowIndex.x = shadowIndices[i].x; // shadowIndex
			mapped[i].shadowIndex.y = shadowIndices[i].y; // shadowStrength
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

		// Register external buffer handles. Actual buffers are updated per-frame via UpdateBufferHandles.
		m_rgLights = graph.RegisterBuffer(nullptr);
		m_rgTileHeaders = graph.RegisterBuffer(nullptr);
		m_rgTileIndices = graph.RegisterBuffer(nullptr);

		const auto initResolved = gpu::ResourceRegistry::ResolvePipeline(m_initPipelineHandle);
		const auto cullResolved = gpu::ResourceRegistry::ResolvePipeline(m_cullPipelineHandle);

		graph.AddComputePass("$Lighting.InitTiles")
		        .ReadBuffer(m_rgLights)
		        .WriteBuffer(m_rgTileHeaders)
		        .WriteBuffer(m_rgTileIndices)
		        .ExecuteCompute(
		                [this, initPipeline = const_cast<void*>(initResolved.state)](PassContext& ctx)
		                {
			                if (!m_lightDataReady)
			                {
				                return;
			                }
			                gpu::CommandList cmd = ctx.recorder.View();

			                // Host-write visibility barrier for the light data buffer.
			                cmd.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);

			                cmd.BindComputePipeline(initPipeline);
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(m_lightPush));
			                cmd.Dispatch(m_lightTileGroups, 1, 1);
		                });

		graph.AddComputePass("$Lighting.BinLights")
		        .ReadBuffer(m_rgLights)
		        .ReadWriteBuffer(m_rgTileHeaders)
		        .ReadWriteBuffer(m_rgTileIndices)
		        .ExecuteCompute(
		                [this, cullPipeline = const_cast<void*>(cullResolved.state)](PassContext& ctx)
		                {
			                if (!m_lightDataReady || m_lightLightGroups == 0)
			                {
				                return;
			                }
			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(cullPipeline);
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(m_lightPush));
			                cmd.Dispatch(m_lightLightGroups, 1, 1);
		                });

		m_rgPassesRegistered = true;
	}

	bool LightingManager::PrepareForRenderGraph(
	        const std::uint32_t frameSlot, const Camera& camera, const gpu::Extent2D extent, FrameConstants& fc, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights)
	{
		if (extent.width == 0 || extent.height == 0)
		{
			DisableForView(fc);
			m_lightDataReady = false;
			return false;
		}

		std::vector<GpuLight> lights;
		BuildLightList(lights, pointLights, spotLights);

		if (lights.empty() && pointLights.empty() && spotLights.empty())
		{
			DisableForView(fc);
			m_lightDataReady = false;
			return false;
		}

		const std::uint32_t tilesX = (extent.width + kTileSizePx - 1u) / kTileSizePx;
		const std::uint32_t tilesY = (extent.height + kTileSizePx - 1u) / kTileSizePx;
		const std::size_t tileCount = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);
		const std::size_t indexCount = tileCount * static_cast<std::size_t>(m_maxLightsPerTile);

		EnsureBuffers(frameSlot, lights.size(), tileCount, indexCount);
		auto& frame = m_buffers[frameSlot];
		if (!lights.empty())
		{
			std::memcpy(frame.lightsMapped, lights.data(), lights.size() * sizeof(GpuLight));
		}
		gpu::ResourceRegistry::FlushMappedBuffer(frame.lightsHandle, 0, static_cast<gpu::DeviceSize>(lights.size()) * sizeof(GpuLight));

		EnsureComputePipeline();

		const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
		const glm::mat4 proj = camera.GetProjectionMatrix(aspect);
		m_lightPush.viewProj = proj * camera.GetViewMatrix();
		m_lightPush.params0 = glm::vec4(camera.GetNearPlane(), 0.5f * static_cast<float>(extent.height) * std::abs(proj[1][1]), static_cast<float>(extent.width), static_cast<float>(extent.height));
		m_lightPush.params1 = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		m_lightPush.params2 = glm::uvec4(m_maxLightsPerTile, 0u, 0u, 0u);
		m_lightPush.lightDataAddr = frame.lightsDeviceAddr;
		m_lightPush.tileHeadersAddr = frame.tileHeadersDeviceAddr;
		m_lightPush.tileLightIndicesAddr = frame.tileIndicesDeviceAddr;

		m_lightTileGroups = static_cast<std::uint32_t>((tileCount + 63u) / 64u);
		m_lightLightGroups = static_cast<std::uint32_t>((lights.size() + 63u) / 64u);
		m_lightDataReady = true;

		fc.tiledLightGridInfo = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		fc.tiledLightBufferOffsets = glm::uvec4(0u, 0u, 0u, m_maxLightsPerTile);

		return true;
	}

	void LightingManager::UpdateBufferHandles(RenderGraph& graph, const std::uint32_t frameSlot) const
	{
		auto& frame = m_buffers[frameSlot];
		graph.UpdateExternalBuffer(m_rgLights, static_cast<void*>(frame.lightsBuffer));
		graph.UpdateExternalBuffer(m_rgTileHeaders, static_cast<void*>(frame.tileHeadersBuffer));
		graph.UpdateExternalBuffer(m_rgTileIndices, static_cast<void*>(frame.tileIndicesBuffer));
	}
} // namespace aether
