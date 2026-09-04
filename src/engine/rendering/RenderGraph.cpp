#include "rendering/RenderGraph.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <numeric>
#include <queue>

#include "gpu/Bda.hpp"
#include "gpu/GpuDeviceFactory.hpp"
#include "gpu/BindlessManager.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/VulkanContext.hpp"
#include "gpu/GpuProfiler.hpp"
#include "vulkan/RenderGraphStorage.hpp"
#include "vulkan/DiagnosticEngine.hpp"

namespace aether
{

	RenderGraph::RenderGraph()
	      : m_storage(std::make_unique<RenderGraphStorage>())
	{
	}

	RenderGraph::~RenderGraph() = default;

	void RenderGraph::Initialize(gpu::Device device, gpu::Allocator allocator)
	{
		m_storage->Initialize(device, allocator);
		m_timingDevice = device;
	}

	void RenderGraph::SetVulkanContext(class VulkanContext* ctx)
	{
		m_storage->SetVulkanContext(ctx);
		m_timingContext = ctx;
		m_timestampPeriodNs = 0.0f;
		if (ctx != nullptr)
		{
			const gpu::Factory::PhysicalDeviceProperties props = gpu::Factory::GetPhysicalDeviceProperties(ctx->GetPhysicalDevice());
			if (props.limits.timestampComputeAndGraphics)
			{
				m_timestampPeriodNs = props.limits.timestampPeriod;
			}
			else
			{
				AE_INFO(LogCategory::Render, "GPU timestamps unsupported on this device; per-pass GPU timings will read zero.");
			}
		}
	}

	// Read back the timings recorded into this slot a full frame cycle ago. The engine
	// has already waited on that frame's fence before handing the slot back, so the
	// results are guaranteed ready and this never blocks.
	//
	// Reading these numbers: a pass's begin timestamp waits for everything submitted
	// before it, so whichever pass runs FIRST in a frame also absorbs whatever drain
	// was left from the previous frame. Measured here, three culls doing identical work
	// read 1.42 / 0.42 / 0.30 ms purely in pass order, and 0.019 / 0.014 / 0.013 in the
	// same order once their workload was removed. Compare a pass against itself across
	// builds, not against its neighbours, and treat the first pass of a frame as an
	// upper bound.
	//
	// Absolute figures also drift with GPU clocks between runs - measured at 13% for an
	// untouched pass - which is easily large enough to invert an A/B verdict, and did
	// once here. Normalise against a pass the change cannot affect ($GTAO_Main and
	// $Skybox are stable to well under 1%) and compare the ratio, not the milliseconds.
	void RenderGraph::ResolveGpuTimings(const std::uint32_t frameSlot)
	{
		GpuTimingFrame& timing = m_gpuTiming[frameSlot];
		if (!timing.pending || timing.pool == nullptr || timing.used == 0 || m_timestampPeriodNs <= 0.0f)
		{
			timing.pending = false;
			return;
		}

		std::vector<std::uint64_t> ticks(timing.used, 0ull);
		const std::uint32_t got = gpu::Factory::GetQueryPoolResults(m_timingDevice, timing.pool, 0, timing.used, ticks);
		timing.pending = false;
		if (got < timing.used)
		{
			return;
		}

		const std::scoped_lock lock(m_debugStateMutex);
		for (std::size_t i = 0; i < timing.timedPasses.size(); ++i)
		{
			const std::uint32_t passIndex = timing.timedPasses[i];
			if (passIndex >= m_passes.size())
			{
				continue;
			}
			const std::uint64_t begin = ticks[i * 2u];
			const std::uint64_t end = ticks[i * 2u + 1u];
			// A wrapped or unwritten pair reads as garbage rather than a negative
			// duration, so drop it instead of reporting a nonsense spike.
			m_passes[passIndex].lastGpuTimeMs = (end > begin) ? (static_cast<float>(end - begin) * m_timestampPeriodNs * 1e-6f) : 0.0f;
		}
	}

	// The timestamp pool for a frame slot is shared by the graphics and the async compute
	// queue, so a vkCmdResetQueryPool recorded on either one is ordered against that queue
	// only - the other queue's writes race it, and validation reports "query not reset" for
	// every timestamp the compute queue takes. Resetting on the HOST removes the ordering
	// question entirely; the caller guarantees the slot is idle on both queues first.
	void RenderGraph::ResetGpuTimings(const std::uint32_t frameSlot, const std::uint32_t passCount)
	{
		if (m_timestampPeriodNs <= 0.0f || m_timingDevice == nullptr)
		{
			return;
		}

		GpuTimingFrame& timing = m_gpuTiming[frameSlot];
		const std::uint32_t needed = passCount * 2u;
		if (timing.capacity < needed)
		{
			if (timing.pool != nullptr)
			{
				gpu::Factory::DestroyQueryPool(m_timingDevice, timing.pool);
			}
			// Round up so a graph that grows by one pass does not reallocate every frame.
			const std::uint32_t capacity = ((needed + 63u) / 64u) * 64u;
			timing.pool = gpu::Factory::CreateQueryPool(m_timingDevice, gpu::Factory::QueryPoolDesc{.type = gpu::Factory::QueryType::Timestamp, .count = capacity});
			timing.capacity = (timing.pool != nullptr) ? capacity : 0u;
		}

		timing.used = 0u;
		timing.timedPasses.clear();
		if (timing.pool != nullptr)
		{
			gpu::Factory::ResetQueryPool(m_timingDevice, timing.pool, 0, timing.capacity);
		}
	}

	void RenderGraph::DestroyGpuTimings()
	{
		if (m_timingDevice == nullptr)
		{
			return;
		}
		for (GpuTimingFrame& timing: m_gpuTiming)
		{
			if (timing.pool != nullptr)
			{
				gpu::Factory::DestroyQueryPool(m_timingDevice, timing.pool);
			}
			timing = {};
		}
	}

	void RenderGraph::Shutdown()
	{
		DestroyGpuTimings();
		const std::scoped_lock lock(m_debugStateMutex);
		m_storage->Shutdown();
		m_externalImages.clear();
		m_passes.clear();
		m_compiled.clear();
		m_lastCulledPasses.clear();
		m_lastImageStates.clear();
		m_lastBufferStates.clear();
		m_compileDirty = true;
	}

	void RenderGraph::BeginFrame(std::uint32_t frameIndex)
	{
		m_frameIndex = frameIndex;
		m_compileDirty = true;
		if (m_diagnosticEngine != nullptr)
		{
			m_diagnosticEngine->BeginFrame(frameIndex);
		}
		m_storage->BeginFrame(frameIndex);
	}

	const FrameStats& RenderGraph::GetFrameStats() const
	{
		return m_storage->GetLastFrameStats();
	}

	void RenderGraph::RemovePass(const std::string& name)
	{
		const std::scoped_lock lock(m_debugStateMutex);
		const auto it = std::ranges::find_if(m_passes, [&](const PassRecord& p) { return p.name == name; });
		if (it != m_passes.end())
		{
			m_passes.erase(it);
			RebuildPreparedDrawListLinks();
			m_compileDirty = true;
		}
	}

	void RenderGraph::RebuildPreparedDrawListLinks()
	{
		for (PreparedDrawListRecord& drawList: m_preparedDrawLists)
		{
			drawList.producerPass = std::numeric_limits<std::size_t>::max();
			drawList.consumerPasses.clear();
		}

		for (std::size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
		{
			const PassRecord& pass = m_passes[passIndex];
			for (const PreparedDrawList drawList: pass.producedDrawLists)
			{
				if (drawList.IsValid() && drawList.id < m_preparedDrawLists.size())
				{
					PreparedDrawListRecord& record = m_preparedDrawLists[drawList.id];
					if (!record.retired)
					{
						record.producerPass = passIndex;
					}
				}
			}
			for (const PreparedDrawList drawList: pass.consumedDrawLists)
			{
				if (drawList.IsValid() && drawList.id < m_preparedDrawLists.size())
				{
					PreparedDrawListRecord& record = m_preparedDrawLists[drawList.id];
					if (!record.retired)
					{
						record.consumerPasses.push_back(passIndex);
					}
				}
			}
		}
	}

	void RenderGraph::Clear()
	{
		const std::scoped_lock lock(m_debugStateMutex);
		// Every transient slot goes, not just the ones that happen to appear in last frame's
		// state map. Services re-declare their transients from scratch after a reset, so a
		// slot left behind here is a full-size image or buffer nothing will ever use again.
		m_storage->ReleaseAllTransients();
		m_transientImageLifetimes.clear();
		m_transientBufferLifetimes.clear();
		m_storage->ClearExternalImages();
		m_externalImages.clear();
		m_storage->ClearExternalBuffers();
		m_externalBuffers.clear();
		m_preparedDrawLists.clear();
		m_blackboard.Clear();
		m_passes.clear();
		m_compiled.clear();
		m_lastCulledPasses.clear();
		m_lastImageStates.clear();
		m_lastBufferStates.clear();
		m_compileDirty = true;
	}

	std::vector<RenderGraph::PassInfo> RenderGraph::GetPasses() const
	{
		const std::scoped_lock lock(m_debugStateMutex);
		auto imageAccessName = [](ImageAccessType type) noexcept -> const char*
		{
			switch (type)
			{
				case ImageAccessType::SampledRead:
					return "Sampled read";
				case ImageAccessType::StorageRead:
					return "Storage read";
				case ImageAccessType::StorageWrite:
					return "Storage write";
				case ImageAccessType::TransferRead:
					return "Transfer read";
				case ImageAccessType::TransferWrite:
					return "Transfer write";
			}
			return "Image access";
		};

		auto bufferAccessName = [](BufferAccessType type) noexcept -> const char*
		{
			switch (type)
			{
				case BufferAccessType::StorageRead:
					return "Storage read";
				case BufferAccessType::StorageWrite:
					return "Storage write";
				case BufferAccessType::StorageReadWrite:
					return "Storage read/write";
				case BufferAccessType::TransferRead:
					return "Transfer read";
				case BufferAccessType::TransferWrite:
					return "Transfer write";
			}
			return "Buffer access";
		};

		auto imageAccessWrites = [](ImageAccessType type) noexcept
		{
			return type == ImageAccessType::StorageWrite || type == ImageAccessType::TransferWrite;
		};

		auto bufferAccessWrites = [](BufferAccessType type) noexcept
		{
			return type == BufferAccessType::StorageWrite || type == BufferAccessType::StorageReadWrite || type == BufferAccessType::TransferWrite;
		};

		std::vector<std::size_t> compiledIndexByPass(m_passes.size(), std::numeric_limits<std::size_t>::max());
		for (std::size_t i = 0; i < m_compiled.size(); ++i)
		{
			if (m_compiled[i].passIndex < compiledIndexByPass.size())
			{
				compiledIndexByPass[m_compiled[i].passIndex] = i;
			}
		}

		std::vector<PassInfo> result;
		result.reserve(m_passes.size());
		for (std::size_t i = 0; i < m_passes.size(); ++i)
		{
			const PassRecord& pass = m_passes[i];
			PassInfo info{
			        .index = i,
			        .compiledIndex = compiledIndexByPass[i] != std::numeric_limits<std::size_t>::max() ? compiledIndexByPass[i] : 0,
			        .name = pass.name,
			        .isGraphics = pass.kind == PassKind::Graphics,
			        .isCompute = pass.kind == PassKind::Compute,
			        .isAsyncCompute = pass.queueClass == QueueClass::AsyncCompute,
			        .isCompiled = compiledIndexByPass[i] != std::numeric_limits<std::size_t>::max(),
			        .isCulled = i < m_lastCulledPasses.size() ? m_lastCulledPasses[i] : false,
			        .isDebugDisabled = pass.debugDisabled,
			        .hasSideEffects = pass.hasSideEffects,
			        .sideEffectReason = pass.sideEffectReason,
			        .logicalDependencies = pass.logicalDependencies,
			        .hasDepthWrite = pass.depthWrite.has_value(),
			        .colorWriteCount = static_cast<std::uint32_t>(pass.colorWrites.size()),
			        .imageAccessCount = static_cast<std::uint32_t>(pass.imageAccesses.size()),
			        .bufferAccessCount = static_cast<std::uint32_t>(pass.bufferAccesses.size()),
			        .extentOverride = pass.extentOverride,
			        .lastCpuTimeMs = pass.lastCpuTimeMs,
			        .lastGpuTimeMs = pass.lastGpuTimeMs,
			};

#ifndef NDEBUG
			info.declaredFile = pass.declaredAt.file_name();
			info.declaredLine = pass.declaredAt.line();
#endif

			auto frameProductLabel = [](const FrameProductRef& product)
			{
				return product.typeName + ":" + product.name;
			};
			for (const FrameProductRef& product: pass.producedFrameProducts)
			{
				info.producedFrameProducts.push_back(frameProductLabel(product));
			}
			for (const FrameProductRef& product: pass.consumedFrameProducts)
			{
				info.consumedFrameProducts.push_back(frameProductLabel(product));
			}
			info.contractWarnings = pass.contractWarnings;

			if (info.isCompiled)
			{
				const CompiledPass& compiled = m_compiled[info.compiledIndex];
				info.preBarrierCount = static_cast<std::uint32_t>(compiled.preBarriers.size());
				info.bufferBarrierCount = static_cast<std::uint32_t>(compiled.bufferBarriers.size());
				info.waitCount = static_cast<std::uint32_t>(compiled.waits.size());
				std::size_t signalled = 0;
				for (const CompiledSignal& sg: compiled.signals)
				{
					signalled += sg.barriers.size();
				}
				info.signalBarrierCount = static_cast<std::uint32_t>(signalled);
				info.splitEventIndex = compiled.signals.empty() ? UINT32_MAX : compiled.signals.front().eventIndex;
			}

			for (const PreparedDrawList drawList: pass.producedDrawLists)
			{
				if (drawList.IsValid() && drawList.id < m_preparedDrawLists.size())
				{
					const PreparedDrawListRecord& record = m_preparedDrawLists[drawList.id];
					if (!record.retired)
					{
						info.producedDrawLists.push_back(record.name);
					}
				}
			}
			for (const PreparedDrawList drawList: pass.consumedDrawLists)
			{
				if (drawList.IsValid() && drawList.id < m_preparedDrawLists.size())
				{
					const PreparedDrawListRecord& record = m_preparedDrawLists[drawList.id];
					if (!record.retired)
					{
						info.consumedDrawLists.push_back(record.name);
					}
				}
			}

			info.resources.reserve(pass.colorWrites.size() + pass.imageAccesses.size() + pass.bufferAccesses.size() + (pass.depthWrite.has_value() ? 1u : 0u));
			for (const AttachmentRef& attachment: pass.colorWrites)
			{
				info.resources.push_back(PassInfo::ResourceAccessInfo{
				        .kind = PassInfo::ResourceAccessInfo::Kind::Image,
				        .id = attachment.image.id,
				        .usage = "Color write",
				        .writes = true,
				});
			}
			if (pass.depthWrite.has_value())
			{
				info.resources.push_back(PassInfo::ResourceAccessInfo{
				        .kind = PassInfo::ResourceAccessInfo::Kind::Image,
				        .id = pass.depthWrite->image.id,
				        .usage = "Depth write",
				        .writes = true,
				});
			}
			for (const ImageAccessRef& access: pass.imageAccesses)
			{
				info.resources.push_back(PassInfo::ResourceAccessInfo{
				        .kind = PassInfo::ResourceAccessInfo::Kind::Image,
				        .id = access.image.id,
				        .usage = imageAccessName(access.type),
				        .writes = imageAccessWrites(access.type),
				});
			}
			for (const BufferAccessRef& access: pass.bufferAccesses)
			{
				info.resources.push_back(PassInfo::ResourceAccessInfo{
				        .kind = PassInfo::ResourceAccessInfo::Kind::Buffer,
				        .id = access.buffer.id,
				        .usage = bufferAccessName(access.type),
				        .writes = bufferAccessWrites(access.type),
				});
			}
			result.push_back(std::move(info));
		}
		return result;
	}

	void RenderGraph::SetPassDebugDisabled(std::string_view name, bool disabled)
	{
		const std::scoped_lock lock(m_debugStateMutex);
		for (PassRecord& pass: m_passes)
		{
			if (pass.name == name)
			{
				pass.debugDisabled = disabled;
				pass.lastCpuTimeMs = disabled ? 0.0f : pass.lastCpuTimeMs;
				pass.lastGpuTimeMs = disabled ? 0.0f : pass.lastGpuTimeMs;
				return;
			}
		}
	}

	bool RenderGraph::IsPassDebugDisabled(std::string_view name) const
	{
		const std::scoped_lock lock(m_debugStateMutex);
		const auto it = std::ranges::find_if(m_passes, [&](const PassRecord& pass) { return pass.name == name; });
		return it != m_passes.end() && it->debugDisabled;
	}

	void RenderGraph::ClearDebugDisabledPasses()
	{
		const std::scoped_lock lock(m_debugStateMutex);
		for (PassRecord& pass: m_passes)
		{
			pass.debugDisabled = false;
		}
	}

	void RenderGraph::EnableAsyncCompute(gpu::Queue computeQueue, std::uint32_t computeQueueFamily)
	{
		m_storage->EnableAsyncCompute(computeQueue, computeQueueFamily);
		m_asyncComputeEnabled = true;
	}

	void RenderGraph::ShutdownAsyncCompute()
	{
		m_storage->ShutdownComputeResources();
		m_asyncComputeEnabled = false;
	}

	bool RenderGraph::HasAsyncComputeWork() const
	{
		if (!m_asyncComputeEnabled)
		{
			return false;
		}
		for (const auto& cp: m_compiled)
		{
			if (cp.queueClass == QueueClass::AsyncCompute)
			{
				return true;
			}
		}
		return false;
	}

	gpu::TimelineSemaphoreHandle RenderGraph::GetComputeTimelineSemaphore() const
	{
		return m_storage->GetCrossQueueTimelineSemaphore();
	}

	std::uint64_t RenderGraph::GetComputeTimelineValue() const
	{
		return m_storage->GetCrossQueueTimelineValue();
	}

	void RenderGraph::SubmitComputeWork(std::uint32_t frameIndex)
	{
		if (HasAsyncComputeWork())
		{
			m_storage->SubmitComputeQueue(frameIndex);
		}
	}

	RGImage RenderGraph::RegisterImage(gpu::Image image, gpu::ImageView view, gpu::ImageAspect aspect)
	{
		const uint32_t idx = m_storage->RegisterExternalImage(image, view, aspect);
		if (idx >= m_externalImages.size())
		{
			m_externalImages.resize(idx + 1);
		}
		m_externalImages[idx] = ExternalImageEntry{
		        .image = image,
		        .view = view,
		        .aspect = aspect,
		};
		const uint32_t id = kFirstExternalId + idx;
		return RGImage{id};
	}

	RGBuffer RenderGraph::RegisterBuffer(gpu::Buffer buffer)
	{
		const uint32_t idx = m_storage->RegisterExternalBuffer(buffer);
		if (idx >= m_externalBuffers.size())
		{
			m_externalBuffers.resize(idx + 1, nullptr);
		}
		m_externalBuffers[idx] = buffer;
		const uint32_t id = kFirstExternalBufferId + idx;
		return RGBuffer{id};
	}

	void RenderGraph::UpdateExternalBuffer(RGBuffer buffer, gpu::Buffer newBuffer)
	{
		const uint32_t idx = ExternalBufferIndex(buffer.id);
		m_storage->UpdateExternalBuffer(idx, newBuffer);
		if (idx < m_externalBuffers.size())
		{
			m_externalBuffers[idx] = newBuffer;
		}
		m_lastBufferStates.erase(buffer.id);
	}

	RGBuffer RenderGraph::CreateTransientBuffer(gpu::DeviceSize size, gpu::BufferUsage usage)
	{
		const uint32_t idx = m_storage->AddTransientBufferSlot(static_cast<VkDeviceSize>(size), static_cast<VkBufferUsageFlags2>(usage));
		m_compileDirty = true;
		return RGBuffer{kFirstTransientBufferId + idx};
	}

	void RenderGraph::ReleaseBuffer(RGBuffer buffer)
	{
		if (!IsTransientBufferId(buffer.id))
		{
			return;
		}
		m_storage->ReleaseTransientBuffer(TransientBufferIndex(buffer.id));
		m_lastBufferStates.erase(buffer.id);
		m_compileDirty = true;
	}

	gpu::Buffer RenderGraph::ResolveBuffer(RGBuffer buffer) const
	{
		if (IsTransientBufferId(buffer.id))
		{
			return m_storage->ResolveTransientBuffer(TransientBufferIndex(buffer.id));
		}
		const uint32_t idx = ExternalBufferIndex(buffer.id);
		return idx < m_externalBuffers.size() ? m_externalBuffers[idx] : nullptr;
	}

	// A pooled buffer is re-created whenever the heap plan changes, so its device address is
	// only valid for the frame it is read in. Callers must fetch it inside Execute.
	gpu::DeviceAddress RenderGraph::GetBufferAddress(RGBuffer buffer) const
	{
		if (!IsTransientBufferId(buffer.id))
		{
			return 0;
		}
		const gpu::BufferHandle handle = m_storage->ResolveTransientBufferHandle(TransientBufferIndex(buffer.id));
		return gpu::GetBufferAddress(handle);
	}

	gpu::Image RenderGraph::ResolveImage(const RGImage image) const
	{
		if (IsTransientId(image.id))
		{
			return m_storage->ResolveTransientImage(TransientIndex(image.id));
		}
		if (image.id == kSwapchainColorId)
		{
			return m_lastFrameContext.target.colorImage;
		}
		if (image.id == kSwapchainDepthId)
		{
			return m_lastFrameContext.target.depthImage;
		}
		return m_storage->GetExternalImage(ExternalIndex(image.id));
	}

	RGImage RenderGraph::CreateTransientImage(const TransientImageDesc& desc)
	{
		const uint32_t idx = m_storage->AddTransientSlot(desc.format, desc.usage, desc.aspect, desc.extent);
		const uint32_t id = kFirstTransientId + idx;
		return RGImage{id};
	}

	RGImage RenderGraph::CreateTransientColor(gpu::Format format, gpu::Extent2D extent, gpu::ImageUsage extraUsage)
	{
		return CreateTransientImage({
		        .format = format,
		        .usage = gpu::ImageUsage::ColorAttachment | extraUsage,
		        .aspect = gpu::ImageAspect::Color,
		        .extent = extent,
		});
	}

	RGImage RenderGraph::CreateTransientDepth(gpu::Format format, gpu::Extent2D extent, gpu::ImageUsage extraUsage)
	{
		return CreateTransientImage({
		        .format = format,
		        .usage = gpu::ImageUsage::DepthStencilAttachment | extraUsage,
		        .aspect = gpu::ImageAspect::Depth,
		        .extent = extent,
		});
	}

	std::uint32_t RenderGraph::EnsureBindlessSampled(RGImage image, gpu::ImageLayout descriptorLayout)
	{
		if (!IsTransientId(image.id))
		{
			return 0xFFFFFFFFu;
		}

		const uint32_t idx = TransientIndex(image.id);
		return m_storage->EnsureBindlessSampled(idx, descriptorLayout);
	}

	std::uint32_t RenderGraph::GetBindlessSampledSlot(RGImage image) const
	{
		if (!IsTransientId(image.id))
		{
			return 0xFFFFFFFFu;
		}

		const uint32_t idx = image.id - kFirstTransientId;
		return m_storage->GetBindlessSampledSlot(idx);
	}

	void RenderGraph::ReleaseImage(RGImage image)
	{
		if (image.id == kSwapchainColorId || image.id == kSwapchainDepthId || image.id == RGImage::kInvalid)
		{
			return;
		}

		if (IsTransientId(image.id))
		{
			const uint32_t idx = TransientIndex(image.id);
			m_storage->ReleaseTransient(idx);
			return;
		}

		const uint32_t idx = ExternalIndex(image.id);
		if (idx < m_externalImages.size())
		{
			m_externalImages[idx] = {};
			m_storage->ReleaseExternal(idx);
		}
	}

	bool RenderGraph::IsPoolableTransient(const uint32_t resourceId) const
	{
		if (IsTransientBufferId(resourceId))
		{
			const uint32_t idx = TransientBufferIndex(resourceId);
			return idx < m_transientBufferLifetimes.size() && m_transientBufferLifetimes[idx].live && m_transientBufferLifetimes[idx].discardsOnFirstUse;
		}
		if (IsTransientId(resourceId))
		{
			const uint32_t idx = TransientIndex(resourceId);
			return idx < m_transientImageLifetimes.size() && m_transientImageLifetimes[idx].live && m_transientImageLifetimes[idx].discardsOnFirstUse;
		}
		return false;
	}

	// A poolable transient may be sitting on memory another resource used earlier in the
	// same frame, so the barrier that re-initialises it has to wait for whatever that was.
	// The plan is not known when barriers are compiled, and one conservative wait per
	// poolable resource per frame is cheaper than threading the plan back into Compile().
	std::uint64_t RenderGraph::FirstUseSrcStage(const uint32_t resourceId) const
	{
		if (IsPoolableTransient(resourceId))
		{
			return static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		}
		return static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
	}

	std::uint64_t RenderGraph::FirstUseSrcAccess(const uint32_t resourceId) const
	{
		if (IsPoolableTransient(resourceId))
		{
			return static_cast<std::uint64_t>(VK_ACCESS_2_MEMORY_WRITE_BIT);
		}
		return 0;
	}

	void RenderGraph::ComputeTransientLifetimes(const std::vector<std::size_t>& sortedIndices, const std::vector<bool>& culled)
	{
		m_transientImageLifetimes.assign(m_storage->GetTransientCount(), TransientLifetime{});
		m_transientBufferLifetimes.assign(m_storage->GetTransientBufferCount(), TransientLifetime{});

		auto slotFor = [this](const uint32_t resourceId) -> TransientLifetime*
		{
			if (IsTransientBufferId(resourceId))
			{
				const uint32_t idx = TransientBufferIndex(resourceId);
				return idx < m_transientBufferLifetimes.size() ? &m_transientBufferLifetimes[idx] : nullptr;
			}
			if (IsTransientId(resourceId))
			{
				const uint32_t idx = TransientIndex(resourceId);
				return idx < m_transientImageLifetimes.size() ? &m_transientImageLifetimes[idx] : nullptr;
			}
			return nullptr;
		};

		// `discards` says the access does not read what was in the resource beforehand. A
		// transient whose first access discards may share memory with a resource whose
		// lifetime has already ended; one whose first access reads may not, because what it
		// would read is last frame's contents and those no longer belong to it.
		//
		// This is the same contract a transient has always had: its contents are undefined
		// until something writes them. Pooling only makes reading un-written regions fail
		// every frame instead of once.
		std::uint32_t compiledIndex = 0;
		auto touch = [&](const uint32_t resourceId, const bool discards)
		{
			TransientLifetime* slot = slotFor(resourceId);
			if (slot == nullptr)
			{
				return;
			}
			if (!slot->live)
			{
				slot->live = true;
				slot->firstPass = compiledIndex;
				slot->discardsOnFirstUse = discards;
			}
			slot->lastPass = compiledIndex;
		};

		for (const std::size_t idx: sortedIndices)
		{
			if (culled[idx])
			{
				continue;
			}
			const PassRecord& pass = m_passes[idx];

			for (const AttachmentRef& a: pass.colorWrites)
			{
				touch(a.image.id, a.loadOp != gpu::LoadOp::Load);
			}
			if (pass.depthWrite.has_value())
			{
				touch(pass.depthWrite->image.id, pass.depthWrite->loadOp != gpu::LoadOp::Load);
			}
			for (const ImageAccessRef& r: pass.imageAccesses)
			{
				const bool discards = r.type == ImageAccessType::StorageWrite || r.type == ImageAccessType::TransferWrite;
				touch(r.image.id, discards);
			}
			for (const BufferAccessRef& r: pass.bufferAccesses)
			{
				const bool discards = r.type == BufferAccessType::StorageWrite || r.type == BufferAccessType::TransferWrite;
				touch(r.buffer.id, discards);
			}

			++compiledIndex;
		}
	}

	void RenderGraph::Compile()
	{
		const std::scoped_lock lock(m_debugStateMutex);
		if (!m_compileDirty)
		{
			return;
		}
		m_compileDirty = false;

		m_storage->ResetEvents();

		const std::size_t N = m_passes.size();
		m_compiled.clear();
		m_compiled.reserve(N);
		for (PassRecord& pass: m_passes)
		{
			pass.contractWarnings.clear();
		}

		auto addPassWarning = [&](const std::size_t passIndex, std::string message)
		{
			if (passIndex < m_passes.size())
			{
				m_passes[passIndex].contractWarnings.push_back(message);
			}
			AE_WARN(LogCategory::Engine, "RenderGraph: {}", message);
		};

		std::unordered_map<std::string_view, std::size_t> passNameCounts;
		for (std::size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
		{
			const PassRecord& pass = m_passes[passIndex];
			++passNameCounts[pass.name];
			if (!pass.execute)
			{
				addPassWarning(passIndex, std::format("pass '{}' has no execute callback.", pass.name));
			}
			if (pass.hasSideEffects && pass.sideEffectReason.empty())
			{
				addPassWarning(passIndex, std::format("pass '{}' declares side effects without a reason string.", pass.name));
			}
			if (!pass.producedDrawLists.empty() && !pass.debugDisabledExecute)
			{
				addPassWarning(passIndex, std::format("pass '{}' produces draw queue state but has no debug-disabled cleanup callback.", pass.name));
			}
		}
		for (const auto& [name, count]: passNameCounts)
		{
			if (count > 1)
			{
				AE_WARN(LogCategory::Engine, "RenderGraph: pass name '{}' is registered {} times. Stable pass names must be unique.", name, count);
			}
		}
		for (const PreparedDrawListRecord& drawList: m_preparedDrawLists)
		{
			if (drawList.retired)
			{
				continue;
			}
			if (drawList.producerPass == std::numeric_limits<std::size_t>::max())
			{
				AE_WARN(LogCategory::Engine, "RenderGraph: PreparedDrawList '{}' has no producer pass.", drawList.name);
			}
			if (drawList.consumerPasses.empty())
			{
				AE_WARN(LogCategory::Engine, "RenderGraph: PreparedDrawList '{}' has no consumer passes.", drawList.name);
			}
		}

		if (m_asyncComputeEnabled)
		{
			for (auto& pass: m_passes)
			{
				if (pass.kind == PassKind::Compute && pass.allowAsyncCompute && pass.queueClass == QueueClass::Graphics && pass.colorWrites.empty() && !pass.depthWrite.has_value() && pass.imageAccesses.empty())
				{
					pass.queueClass = QueueClass::AsyncCompute;
				}
			}
		}

		std::vector<std::vector<std::size_t>> adj(N);
		std::vector<std::size_t> inDegree(N, 0);

		auto addEdge = [&](std::size_t from, std::size_t to)
		{
			if (from == to)
			{
				return;
			}
			if (std::ranges::find(adj[from], to) != adj[from].end())
			{
				return;
			}
			adj[from].push_back(to);
			++inDegree[to];
		};

		auto passMatchesName = [](const PassRecord& pass, const std::string& requestedName) -> bool
		{
			return pass.name == requestedName;
		};

		for (std::size_t i = 0; i < N; ++i)
		{
			for (const std::string& dependencyName: m_passes[i].logicalDependencies)
			{
				const auto it = std::ranges::find_if(m_passes, [&](const PassRecord& candidate) { return passMatchesName(candidate, dependencyName); });
				if (it == m_passes.end())
				{
					AE_WARN(LogCategory::Engine, "RenderGraph: pass '{}' depends on '{}', but no matching pass was found.", m_passes[i].name, dependencyName);
					continue;
				}
				addEdge(static_cast<std::size_t>(std::distance(m_passes.begin(), it)), i);
			}
		}

		for (const PreparedDrawListRecord& drawList: m_preparedDrawLists)
		{
			if (drawList.retired || drawList.producerPass == std::numeric_limits<std::size_t>::max())
			{
				continue;
			}
			for (const std::size_t consumerPass: drawList.consumerPasses)
			{
				if (consumerPass >= N)
				{
					continue;
				}
				addEdge(drawList.producerPass, consumerPass);
			}
		}

		auto frameProductKey = [](const FrameProductRef& product)
		{
			return std::to_string(product.type.hash_code()) + ":" + product.name;
		};
		const auto blackboardProducts = m_blackboard.GetProducts();
		auto frameProductInfoKey = [](std::string_view typeName, std::string_view name)
		{
			std::string key{typeName};
			key.push_back(':');
			key.append(name);
			return key;
		};
		auto frameProductRefInfoKey = [&](const FrameProductRef& product)
		{
			return frameProductInfoKey(product.typeName, product.name);
		};
		auto blackboardExternalProducer = [&](const FrameProductRef& product)
		{
			return std::ranges::any_of(blackboardProducts,
			        [&](const FrameBlackboard::ProductInfo& info) { return info.name == product.name && info.typeName == product.typeName && !info.producerPass.empty() && info.metadata.source != FrameBlackboard::ProductSource::GraphPass; });
		};

		std::unordered_map<std::string, std::vector<std::string>> frameProductValidationWarnings;
		auto validateBlackboardProduct = [&]<typename T>(const FrameBlackboard::ProductInfo& info, std::vector<std::string>& warnings)
		{
			if (info.typeName != typeid(T).name())
			{
				return false;
			}
			const T* product = m_blackboard.TryGet<T>(info.name);
			if (product == nullptr)
			{
				warnings.push_back(std::format("frame product '{}:{}' exists in metadata but has no typed storage.", info.typeName, info.name));
				return true;
			}
			FrameProductTraits<T>::Validate(info.name, *product, info.metadata, warnings);
			return true;
		};
		for (const FrameBlackboard::ProductInfo& info: blackboardProducts)
		{
			std::vector<std::string> warnings;
			const bool knownProduct = validateBlackboardProduct.template operator()<PreparedDrawList>(info, warnings) || validateBlackboardProduct.template operator()<MainViewProduct>(info, warnings)
			                          || validateBlackboardProduct.template operator()<FrameTextureProduct>(info, warnings) || validateBlackboardProduct.template operator()<FrameTextureArrayProduct>(info, warnings)
			                          || validateBlackboardProduct.template operator()<LocalShadowProduct>(info, warnings) || validateBlackboardProduct.template operator()<LightBuffersProduct>(info, warnings);
			if (knownProduct && !warnings.empty())
			{
				frameProductValidationWarnings.emplace(frameProductInfoKey(info.typeName, info.name), std::move(warnings));
			}
		}
		auto addProductValidationWarnings = [&](const std::size_t passIndex, const FrameProductRef& product, std::string_view direction)
		{
			const auto warningsIt = frameProductValidationWarnings.find(frameProductRefInfoKey(product));
			if (warningsIt == frameProductValidationWarnings.end())
			{
				return;
			}
			for (const std::string& warning: warningsIt->second)
			{
				addPassWarning(passIndex, std::format("pass '{}' {} invalid product '{}:{}': {}", m_passes[passIndex].name, direction, product.typeName, product.name, warning));
			}
		};

		std::unordered_map<std::string, std::size_t> frameProductProducers;
		std::unordered_map<std::string, std::size_t> frameProductConsumerCounts;
		for (std::size_t i = 0; i < N; ++i)
		{
			for (const FrameProductRef& product: m_passes[i].producedFrameProducts)
			{
				addProductValidationWarnings(i, product, "produces");
				const std::string key = frameProductKey(product);
				const auto [it, inserted] = frameProductProducers.emplace(key, i);
				if (!inserted && it->second != i)
				{
					addPassWarning(i, std::format("frame product '{}:{}' has multiple producers ('{}' and '{}'). Keeping the first producer for ordering.", product.typeName, product.name, m_passes[it->second].name, m_passes[i].name));
				}
			}
			for (const FrameProductRef& product: m_passes[i].consumedFrameProducts)
			{
				addProductValidationWarnings(i, product, "consumes");
				++frameProductConsumerCounts[frameProductKey(product)];
			}
		}

		for (std::size_t consumerPass = 0; consumerPass < N; ++consumerPass)
		{
			for (const FrameProductRef& product: m_passes[consumerPass].consumedFrameProducts)
			{
				const auto it = frameProductProducers.find(frameProductKey(product));
				if (it == frameProductProducers.end())
				{
					if (!blackboardExternalProducer(product))
					{
						addPassWarning(consumerPass, std::format("pass '{}' consumes frame product '{}:{}', but no producer pass was found.", m_passes[consumerPass].name, product.typeName, product.name));
					}
					continue;
				}
				addEdge(it->second, consumerPass);
			}
		}

		for (const auto& [key, producerPass]: frameProductProducers)
		{
			if (frameProductConsumerCounts.contains(key))
			{
				continue;
			}
			for (const FrameProductRef& product: m_passes[producerPass].producedFrameProducts)
			{
				if (frameProductKey(product) == key)
				{
					addPassWarning(producerPass, std::format("pass '{}' produces frame product '{}:{}' with no consumer pass.", m_passes[producerPass].name, product.typeName, product.name));
					break;
				}
			}
		}

		auto passWrites = [&](std::size_t idx, uint32_t resId) -> bool
		{
			for (const AttachmentRef& a: m_passes[idx].colorWrites)
			{
				if (a.image.id == resId)
				{
					return true;
				}
			}
			if (m_passes[idx].depthWrite.has_value() && m_passes[idx].depthWrite->image.id == resId)
			{
				return true;
			}
			for (const ImageAccessRef& a: m_passes[idx].imageAccesses)
			{
				if (a.image.id == resId && (a.type == ImageAccessType::StorageWrite || a.type == ImageAccessType::TransferWrite))
				{
					return true;
				}
			}
			for (const BufferAccessRef& a: m_passes[idx].bufferAccesses)
			{
				if (a.buffer.id == resId && (a.type == BufferAccessType::StorageWrite || a.type == BufferAccessType::StorageReadWrite || a.type == BufferAccessType::TransferWrite))
				{
					return true;
				}
			}
			return false;
		};

		auto passAccesses = [&](std::size_t idx, uint32_t resId) -> bool
		{
			if (passWrites(idx, resId))
			{
				return true;
			}
			for (const ImageAccessRef& r: m_passes[idx].imageAccesses)
			{
				if (r.image.id == resId)
				{
					return true;
				}
			}
			for (const BufferAccessRef& r: m_passes[idx].bufferAccesses)
			{
				if (r.buffer.id == resId)
				{
					return true;
				}
			}
			return false;
		};

		for (std::size_t i = 0; i < N; ++i)
		{
			for (std::size_t j = i + 1; j < N; ++j)
			{
				bool dependent = false;
				for (const AttachmentRef& a: m_passes[i].colorWrites)
				{
					if (passAccesses(j, a.image.id))
					{
						dependent = true;
						break;
					}
				}
				if (!dependent && m_passes[i].depthWrite.has_value() && passAccesses(j, m_passes[i].depthWrite->image.id))
				{
					dependent = true;
				}
				if (!dependent)
				{
					for (const ImageAccessRef& ia: m_passes[i].imageAccesses)
					{
						if ((ia.type == ImageAccessType::StorageWrite || ia.type == ImageAccessType::TransferWrite) && passAccesses(j, ia.image.id))
						{
							dependent = true;
							break;
						}
					}
				}
				if (!dependent)
				{
					for (const BufferAccessRef& ba: m_passes[i].bufferAccesses)
					{
						if ((ba.type == BufferAccessType::StorageWrite || ba.type == BufferAccessType::StorageReadWrite || ba.type == BufferAccessType::TransferWrite) && passAccesses(j, ba.buffer.id))
						{
							dependent = true;
							break;
						}
					}
				}
				if (dependent)
				{
					addEdge(i, j);
				}
			}
		}

		struct ReadyCompare
		{
			const std::vector<PassRecord>& passes;
			const std::vector<std::vector<std::size_t>>& adj;

			[[nodiscard]] bool operator()(std::size_t a, std::size_t b) const
			{
				const bool aAC = passes[a].queueClass == QueueClass::AsyncCompute;
				const bool bAC = passes[b].queueClass == QueueClass::AsyncCompute;
				if (aAC != bAC)
				{
					return bAC;
				}

				return a > b;
			}
		};

		const ReadyCompare readyCmp{.passes = m_passes, .adj = adj};
		std::priority_queue<std::size_t, std::vector<std::size_t>, ReadyCompare> ready(readyCmp);
		for (std::size_t i = 0; i < N; ++i)
		{
			if (inDegree[i] == 0)
			{
				ready.push(i);
			}
		}

		std::vector<std::size_t> sortedIndices;
		sortedIndices.reserve(N);
		while (!ready.empty())
		{
			const std::size_t cur = ready.top();
			ready.pop();
			sortedIndices.push_back(cur);
			for (const std::size_t next: adj[cur])
			{
				if (--inDegree[next] == 0)
				{
					ready.push(next);
				}
			}
		}

		if (sortedIndices.size() != N)
		{
#ifndef NDEBUG
			for (std::size_t i = 0; i < N; ++i)
			{
				const auto it = std::find(sortedIndices.begin(), sortedIndices.end(), static_cast<uint32_t>(i));
				if (it == sortedIndices.end())
				{
					const auto& p = m_passes[i];
					AE_WARN(LogCategory::Engine,
					        "RenderGraph: cycle detected - falling back to declaration order. "
					        "Unreachable pass '{}' declared at {}:{}",
					        p.name,
					        p.declaredAt.file_name(),
					        p.declaredAt.line());
					break;
				}
			}
#else
			AE_WARN(LogCategory::Engine, "RenderGraph: cycle detected - falling back to declaration order.");
#endif
			sortedIndices.resize(N);
			std::ranges::iota(sortedIndices, 0);
		}

		// and Execute() never dispatches their work.

		std::vector<bool> passCulledByPassIdx(N, false);
		{
			std::unordered_map<uint32_t, std::vector<std::size_t>> resourceReaders;
			for (std::size_t i = 0; i < N; ++i)
			{
				const std::size_t passIdx = sortedIndices[i];
				const PassRecord& pass = m_passes[passIdx];

				auto recordRead = [&](uint32_t resId)
				{
					resourceReaders[resId].push_back(i);
				};

				auto recordFrameProductReads = [&]<typename T>(const FrameProductRef& product)
				{
					if (product.type != std::type_index(typeid(T)))
					{
						return false;
					}

					if (const T* value = m_blackboard.TryGet<T>(product.name))
					{
						std::vector<RGImage> sampledImages;
						FrameProductShaderResources<T>::AppendSampledImages(*value, sampledImages);
						for (const RGImage image: sampledImages)
						{
							recordRead(image.id);
						}
					}
					return true;
				};

				for (const ImageAccessRef& r: pass.imageAccesses)
				{
					if (r.type != ImageAccessType::StorageWrite && r.type != ImageAccessType::TransferWrite)
					{
						recordRead(r.image.id);
					}
				}

				// Buffer reads count too. Graph-owned buffers are not automatically live the
				// way external ones are, so a producer that only feeds a buffer would look
				// dead and take the rest of its chain with it.
				for (const BufferAccessRef& r: pass.bufferAccesses)
				{
					if (r.type != BufferAccessType::StorageWrite && r.type != BufferAccessType::TransferWrite)
					{
						recordRead(r.buffer.id);
					}
				}

				for (const FrameProductRef& product: pass.consumedFrameProducts)
				{
					(void) (recordFrameProductReads.template operator()<FrameTextureProduct>(product) || recordFrameProductReads.template operator()<FrameTextureArrayProduct>(product)
					        || recordFrameProductReads.template operator()<LocalShadowProduct>(product));
				}

				for (const AttachmentRef& a: pass.colorWrites)
				{
					if (a.loadOp == gpu::LoadOp::Load)
					{
						recordRead(a.image.id);
					}
				}

				if (pass.depthWrite.has_value() && pass.depthWrite->loadOp == gpu::LoadOp::Load)
				{
					recordRead(pass.depthWrite->image.id);
				}
			}

			for (std::size_t i = 0; i < N; ++i)
			{
				const std::size_t passIdx = sortedIndices[i];
				const PassRecord& pass = m_passes[passIdx];
				if (pass.hasSideEffects)
				{
					continue;
				}

				std::vector<uint32_t> writeTargets;
				writeTargets.reserve(pass.colorWrites.size());
				for (const AttachmentRef& a: pass.colorWrites)
				{
					writeTargets.push_back(a.image.id);
				}
				if (pass.depthWrite.has_value())
				{
					writeTargets.push_back(pass.depthWrite->image.id);
				}
				for (const ImageAccessRef& ia: pass.imageAccesses)
				{
					if (ia.type == ImageAccessType::StorageWrite || ia.type == ImageAccessType::TransferWrite)
					{
						writeTargets.push_back(ia.image.id);
					}
				}
				for (const BufferAccessRef& ba: pass.bufferAccesses)
				{
					if (ba.type == BufferAccessType::StorageWrite || ba.type == BufferAccessType::StorageReadWrite || ba.type == BufferAccessType::TransferWrite)
					{
						writeTargets.push_back(ba.buffer.id);
					}
				}

				if (writeTargets.empty())
				{
					continue;
				}

				// Side-effect targets are never dead
				auto isSideEffect = [&](uint32_t resId) -> bool
				{
					// Anything the graph does not own may be read by code it cannot see, so a
					// write to it is never dead. Transients - images and buffers alike - are
					// owned by the graph and stand or fall on whether a later pass reads them.
					return resId == kSwapchainColorId || resId == kSwapchainDepthId || (resId >= kFirstExternalId && resId < kFirstTransientId);
				};

				bool hasSideEffect = false;
				for (const uint32_t resId: writeTargets)
				{
					if (isSideEffect(resId))
					{
						hasSideEffect = true;
						break;
					}
				}
				if (hasSideEffect)
				{
					continue;
				}

				bool anyReaderFound = false;
				for (const uint32_t resId: writeTargets)
				{
					const auto it = resourceReaders.find(resId);
					if (it != resourceReaders.end())
					{
						for (std::size_t readerPos: it->second)
						{
							if (readerPos > i)
							{
								anyReaderFound = true;
								break;
							}
						}
					}
					if (anyReaderFound)
					{
						break;
					}
				}

				if (!anyReaderFound)
				{
					passCulledByPassIdx[passIdx] = true;

					// Reported in every configuration, not just debug builds. A pass that
					// silently stops executing is one of the hardest things to diagnose
					// here - the work simply is not there, with no error and no visual
					// clue beyond whatever it should have produced being stale or blank.
					// It has cost real time twice: once when the last bloom upsample
					// vanished (see the tonemap pass's ReadTexture of mip 0), and again
					// on a pass whose only reader took its slot through a push constant,
					// which the graph cannot see.
					//
					// Once per pass name, on the transition into being culled, because
					// the graph is rebuilt every frame and this must never become spam.
					if (m_reportedCulledPasses.insert(std::string(pass.name)).second)
					{
						std::string deadResources;
						for (uint32_t resId: writeTargets)
						{
							if (!deadResources.empty())
							{
								deadResources += ", ";
							}
							deadResources += std::to_string(resId);
						}
						AE_WARN(LogCategory::Engine,
						        "RenderGraph: pass '{}' writes RGImage(s) {} that no later pass reads, so it is culled and will not execute. "
						        "If something does read it, declare that read - a slot passed through a push constant is invisible here.",
						        pass.name,
						        deadResources);
					}
				}
				else
				{
					// Reading again is a fresh event worth hearing about.
					m_reportedCulledPasses.erase(std::string(pass.name));
				}
			}
		}
		m_lastCulledPasses = passCulledByPassIdx;

		// Validate queue grouping: all async-compute passes must come before
		{
			bool seenGraphics = false;
			for (const std::size_t idx: sortedIndices)
			{
				if (passCulledByPassIdx[idx])
				{
					continue;
				}
				const auto qc = m_passes[idx].queueClass;
				if (qc == QueueClass::Graphics)
				{
					seenGraphics = true;
				}
				else if (qc == QueueClass::AsyncCompute && seenGraphics)
				{
					AE_WARN(LogCategory::Engine,
					        "RenderGraph: async-compute pass '{}' appears after a graphics pass. "
					        "All async-compute passes must be declared before graphics passes for "
					        "single-submission-per-queue scheduling. Falling back to graphics queue.",
					        m_passes[idx].name);
					m_passes[idx].queueClass = QueueClass::Graphics;
				}
			}
		}

		ComputeTransientLifetimes(sortedIndices, passCulledByPassIdx);

		std::unordered_map<uint32_t, ResourceState> states;
		for (const auto& [id, s]: m_lastImageStates)
		{
			if (IsTransientId(id))
			{
				const uint32_t idx = TransientIndex(id);
				if (!m_storage->IsTransientSlotValid(idx))
				{
					continue;
				}
				// A poolable transient owns nothing between frames - the bytes may belong to
				// another resource by the time the frame comes round again - so it must not
				// inherit last frame's layout. Its first use re-initialises it from scratch.
				if (IsPoolableTransient(id))
				{
					continue;
				}
			}
			states[id] = s;
		}

		// Pre-seed swapchain images with their resting layout.
		states[kSwapchainColorId] = {
		        .layout = gpu::ImageLayout::ColorAttachment,
		        .writeStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT),
		        .writeAccess = static_cast<std::uint64_t>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
		};
		states[kSwapchainDepthId] = {
		        .layout = gpu::ImageLayout::DepthAttachment,
		        .writeStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT),
		        .writeAccess = static_cast<std::uint64_t>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
		};

		std::unordered_map<uint32_t, BufferState> bufferStates;
		for (const auto& [id, s]: m_lastBufferStates)
		{
			if (IsPoolableTransient(id))
			{
				continue;
			}
			bufferStates[id] = s;
		}

		for (const std::size_t idx: sortedIndices)
		{
			if (passCulledByPassIdx[idx])
			{
				continue;
			}

			const PassRecord& pass = m_passes[idx];
			CompiledPass cp;
			cp.passIndex = idx;
			cp.queueClass = pass.queueClass;

			for (const AttachmentRef& a: pass.colorWrites)
			{
				const uint32_t resId = a.image.id;
#ifndef NDEBUG
				// A depth-format resource must never reach a color-attachment slot:
				{
					gpu::ImageAspect attachmentAspect = gpu::ImageAspect::Color;
					if (IsTransientId(resId))
					{
						attachmentAspect = m_storage->ResolveTransientAspect(TransientIndex(resId));
					}
					else if (resId == kSwapchainDepthId)
					{
						attachmentAspect = gpu::ImageAspect::Depth;
					}
					else if (resId != kSwapchainColorId)
					{
						const uint32_t extIdx = ExternalIndex(resId);
						if (extIdx < m_externalImages.size())
						{
							attachmentAspect = m_externalImages[extIdx].aspect;
						}
					}
					AE_ASSERT(attachmentAspect != gpu::ImageAspect::Depth, std::format("RenderGraph: depth-format resource id={} declared as a COLOR attachment in pass '{}' (dangling/aliased external handle?)", resId, pass.name));
				}
#endif
				constexpr gpu::ImageLayout kTarget = gpu::ImageLayout::ColorAttachment;
				constexpr auto kDstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
				constexpr auto kDstWrite = static_cast<std::uint64_t>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
				constexpr auto kDstReadWrite = static_cast<std::uint64_t>(VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

				// LOAD_OP_LOAD reads the attachment, so the barrier that hands it to this pass
				// has to allow COLOR_ATTACHMENT_READ as well - on FIRST use too, which used to
				// take the write-only path below and trip a READ_AFTER_WRITE hazard.
				const bool loadRead = (a.loadOp == gpu::LoadOp::Load);

				const auto it = states.find(resId);
				if (it != states.end())
				{
					const ResourceState& s = it->second;
					const bool layoutChange = (s.layout != kTarget);
					if (layoutChange || loadRead)
					{
						const bool isWAR = (s.writeStage == 0 && s.readStages != 0);
						cp.preBarriers.push_back({
						        .resourceId = resId,
						        .oldLayout = s.layout,
						        .newLayout = kTarget,
						        .srcStage = isWAR ? s.readStages : s.writeStage,
						        .srcAccess = isWAR ? 0u : s.writeAccess,
						        .dstStage = kDstStage,
						        .dstAccess = loadRead ? kDstReadWrite : kDstWrite,
						        .aspect = gpu::ImageAspect::Color,
						        .isWAR = isWAR,
						});
					}
				}
				else
				{
					cp.preBarriers.push_back({
					        .resourceId = resId,
					        .oldLayout = gpu::ImageLayout::Undefined,
					        .newLayout = kTarget,
					        .srcStage = FirstUseSrcStage(resId),
					        .srcAccess = FirstUseSrcAccess(resId),
					        .dstStage = kDstStage,
					        .dstAccess = loadRead ? kDstReadWrite : kDstWrite,
					        .aspect = gpu::ImageAspect::Color,
					});
				}

				states[resId] = {
				        .layout = kTarget,
				        .writeStage = kDstStage,
				        .writeAccess = kDstWrite,
				        .readStages = 0,
				};
			}

			if (pass.depthWrite.has_value())
			{
				const AttachmentRef& da = *pass.depthWrite;
				const uint32_t resId = da.image.id;
				constexpr gpu::ImageLayout kTarget = gpu::ImageLayout::DepthAttachment;
				constexpr auto kDepthStages = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
				constexpr auto kDepthWrite = static_cast<std::uint64_t>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
				constexpr auto kDepthReadWrite = static_cast<std::uint64_t>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

				const auto it = states.find(resId);
				if (it != states.end())
				{
					const ResourceState& s = it->second;
					const bool layoutChange = (s.layout != kTarget);
					const bool loadRead = (da.loadOp == gpu::LoadOp::Load);
					if (layoutChange || loadRead)
					{
						const bool isWAR = (s.writeStage == 0 && s.readStages != 0);
						cp.preBarriers.push_back({
						        .resourceId = resId,
						        .oldLayout = s.layout,
						        .newLayout = kTarget,
						        .srcStage = isWAR ? s.readStages : s.writeStage,
						        .srcAccess = isWAR ? 0u : s.writeAccess,
						        .dstStage = kDepthStages,
						        .dstAccess = loadRead ? kDepthReadWrite : kDepthWrite,
						        .aspect = gpu::ImageAspect::Depth,
						        .isWAR = isWAR,
						});
					}
				}
				else
				{
					cp.preBarriers.push_back({
					        .resourceId = resId,
					        .oldLayout = gpu::ImageLayout::Undefined,
					        .newLayout = kTarget,
					        .srcStage = FirstUseSrcStage(resId),
					        .srcAccess = FirstUseSrcAccess(resId),
					        .dstStage = kDepthStages,
					        .dstAccess = kDepthWrite,
					        .aspect = gpu::ImageAspect::Depth,
					});
				}

				states[resId] = {
				        .layout = kTarget,
				        .writeStage = kDepthStages,
				        .writeAccess = kDepthWrite,
				        .readStages = 0,
				};
			}

			for (const ImageAccessRef& r: pass.imageAccesses)
			{
				const uint32_t resId = r.image.id;

				gpu::ImageLayout targetLayout = gpu::ImageLayout::ShaderReadOnly;
				auto dstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
				auto dstAccess = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

				switch (r.type)
				{
					case ImageAccessType::SampledRead:
						dstStage = (pass.kind == PassKind::Compute) ? static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) : static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
						dstAccess = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
						targetLayout = gpu::ImageLayout::ShaderReadOnly;
						break;
					case ImageAccessType::StorageRead:
						dstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
						dstAccess = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
						targetLayout = gpu::ImageLayout::General;
						break;
					case ImageAccessType::StorageWrite:
						dstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
						dstAccess = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
						targetLayout = gpu::ImageLayout::General;
						break;
					case ImageAccessType::TransferRead:
						dstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COPY_BIT);
						dstAccess = static_cast<std::uint64_t>(VK_ACCESS_2_TRANSFER_READ_BIT);
						targetLayout = gpu::ImageLayout::TransferSrc;
						break;
					case ImageAccessType::TransferWrite:
						dstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COPY_BIT);
						dstAccess = static_cast<std::uint64_t>(VK_ACCESS_2_TRANSFER_WRITE_BIT);
						targetLayout = gpu::ImageLayout::TransferDst;
						break;
				}

				const bool isRead = (r.type != ImageAccessType::StorageWrite && r.type != ImageAccessType::TransferWrite);

				auto it = states.find(resId);

				// RAR: already in the right layout and only reads since last write -> no barrier
				if (isRead && it != states.end() && it->second.layout == targetLayout && it->second.writeStage == 0)
				{
					it->second.readStages |= dstStage;
					continue;
				}

				std::uint64_t srcStage = 0;
				std::uint64_t srcAccess = 0;
				gpu::ImageLayout oldLayout = gpu::ImageLayout::Undefined;
				bool isWAR = false;

				if (it != states.end())
				{
					const ResourceState& s = it->second;
					oldLayout = s.layout;

					isWAR = (s.writeStage == 0 && s.readStages != 0);
					if (isWAR)
					{
						srcStage = s.readStages;
						srcAccess = 0;
					}
					else
					{
						srcStage = s.writeStage;
						srcAccess = s.writeAccess;
					}
				}
				else
				{
					oldLayout = gpu::ImageLayout::Undefined;
					srcStage = FirstUseSrcStage(resId);
					srcAccess = FirstUseSrcAccess(resId);
				}

				gpu::ImageAspect aspect = gpu::ImageAspect::Color;
				if (IsTransientId(resId))
				{
					aspect = m_storage->ResolveTransientAspect(TransientIndex(resId));
				}
				else if (resId == kSwapchainDepthId)
				{
					aspect = gpu::ImageAspect::Depth;
				}
				else
				{
					const uint32_t extIdx = ExternalIndex(resId);
					if (extIdx < m_externalImages.size())
					{
						aspect = m_externalImages[extIdx].aspect;
					}
				}

				cp.preBarriers.push_back({
				        .resourceId = resId,
				        .oldLayout = oldLayout,
				        .newLayout = targetLayout,
				        .srcStage = srcStage,
				        .srcAccess = srcAccess,
				        .dstStage = dstStage,
				        .dstAccess = dstAccess,
				        .aspect = aspect,
				        .isWAR = isWAR,
				});

				if (isRead)
				{
					states[resId] = {
					        .layout = targetLayout,
					        .writeStage = 0,
					        .writeAccess = 0,
					        .readStages = dstStage,
					};
				}
				else
				{
					states[resId] = {
					        .layout = targetLayout,
					        .writeStage = dstStage,
					        .writeAccess = dstAccess,
					        .readStages = 0,
					};
				}
			}

			for (const BufferAccessRef& r: pass.bufferAccesses)
			{
				const uint32_t resId = r.buffer.id;

				const bool isComputePass = (pass.kind == PassKind::Compute);
				constexpr auto kComputeStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				constexpr auto kGraphicsStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
				constexpr auto kStorageRead = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
				constexpr auto kStorageWrite = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
				constexpr auto kTransferStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COPY_BIT);
				constexpr auto kTransferRead = static_cast<std::uint64_t>(VK_ACCESS_2_TRANSFER_READ_BIT);
				constexpr auto kTransferWrite = static_cast<std::uint64_t>(VK_ACCESS_2_TRANSFER_WRITE_BIT);

				std::uint64_t dstStage = 0;
				std::uint64_t dstAccess = 0;
				bool isRead = false;
				bool isReadWrite = false;

				switch (r.type)
				{
					case BufferAccessType::StorageRead:
						dstStage = isComputePass ? kComputeStage : kGraphicsStage;
						dstAccess = kStorageRead;
						isRead = true;
						break;
					case BufferAccessType::StorageWrite:
						dstStage = isComputePass ? kComputeStage : kGraphicsStage;
						dstAccess = kStorageWrite;
						isRead = false;
						break;
					case BufferAccessType::StorageReadWrite:
						dstStage = isComputePass ? kComputeStage : kGraphicsStage;
						dstAccess = kStorageRead | kStorageWrite;
						isRead = false;
						isReadWrite = true;
						break;
					case BufferAccessType::TransferRead:
						dstStage = kTransferStage;
						dstAccess = kTransferRead;
						isRead = true;
						break;
					case BufferAccessType::TransferWrite:
						dstStage = kTransferStage;
						dstAccess = kTransferWrite;
						isRead = false;
						break;
				}

				auto it = bufferStates.find(resId);

				if (isRead && it != bufferStates.end() && it->second.writeStage == 0)
				{
					it->second.readStages |= dstStage;
					continue;
				}

				std::uint64_t srcStage = 0;
				std::uint64_t srcAccess = 0;
				bool isWAR = false;

				if (it != bufferStates.end())
				{
					const BufferState& s = it->second;
					isWAR = (s.writeStage == 0 && s.readStages != 0);
					srcStage = isWAR ? s.readStages : s.writeStage;
					srcAccess = isWAR ? 0u : s.writeAccess;
				}
				else
				{
					srcStage = FirstUseSrcStage(resId);
					srcAccess = FirstUseSrcAccess(resId);
				}

				cp.bufferBarriers.push_back({
				        .resourceId = resId,
				        .srcStage = srcStage,
				        .srcAccess = srcAccess,
				        .dstStage = dstStage,
				        .dstAccess = dstAccess,
				        .isWAR = isWAR,
				});

				if (isRead && !isReadWrite)
				{
					bufferStates[resId] = {.writeStage = 0, .writeAccess = 0, .readStages = dstStage};
				}
				else
				{
					bufferStates[resId] = {.writeStage = dstStage, .writeAccess = dstAccess, .readStages = 0};
				}
			}

			m_compiled.push_back(std::move(cp));
		}

		std::swap(m_lastImageStates, states);
		std::swap(m_lastBufferStates, bufferStates);

		std::unordered_map<uint32_t, std::size_t> resLastWriterCi;
		for (std::size_t ci = 0; ci < m_compiled.size(); ++ci)
		{
			const PassRecord& pass = m_passes[m_compiled[ci].passIndex];

			auto recordWrite = [&](uint32_t resId)
			{
				resLastWriterCi[resId] = ci;
			};

			for (const AttachmentRef& a: pass.colorWrites)
			{
				recordWrite(a.image.id);
			}
			if (pass.depthWrite.has_value())
			{
				recordWrite(pass.depthWrite->image.id);
			}
			for (const ImageAccessRef& ia: pass.imageAccesses)
			{
				if (ia.type == ImageAccessType::StorageWrite || ia.type == ImageAccessType::TransferWrite)
				{
					recordWrite(ia.image.id);
				}
			}
		}

		// Eligible barrier check: producers must be ≥2 apart, no intermediate
		for (std::size_t ci = 0; ci < m_compiled.size(); ++ci)
		{
			auto& cp = m_compiled[ci];

			std::vector<CompiledBarrier> unsplittable;
			unsplittable.reserve(cp.preBarriers.size());

			// Which event this consumer already took from a given producer, so several
			// resources coming from the same producer share one event rather than one each.
			std::unordered_map<std::size_t, std::uint32_t> producerToWait;

			for (const CompiledBarrier& b: cp.preBarriers)
			{
				if (b.isWAR)
				{
					unsplittable.push_back(b);
					continue;
				}

				const auto lwIt = resLastWriterCi.find(b.resourceId);
				if (lwIt == resLastWriterCi.end())
				{
					unsplittable.push_back(b);
					continue;
				}

				const std::size_t producerCi = lwIt->second;

				if (m_compiled[producerCi].queueClass != m_compiled[ci].queueClass)
				{
					unsplittable.push_back(b);
					continue;
				}

				if (producerCi + 1 >= ci)
				{
					unsplittable.push_back(b);
					continue;
				}

				bool intermediateWrite = false;
				for (std::size_t ic = producerCi + 1; ic < ci; ++ic)
				{
					const PassRecord& ipass = m_passes[m_compiled[ic].passIndex];

					auto checkWrite = [&](uint32_t resId) -> bool
					{
						return resId == b.resourceId;
					};

					for (const AttachmentRef& a: ipass.colorWrites)
					{
						if (checkWrite(a.image.id))
						{
							intermediateWrite = true;
							break;
						}
					}
					if (!intermediateWrite && ipass.depthWrite.has_value() && checkWrite(ipass.depthWrite->image.id))
					{
						intermediateWrite = true;
					}
					if (!intermediateWrite)
					{
						for (const ImageAccessRef& ia: ipass.imageAccesses)
						{
							if ((ia.type == ImageAccessType::StorageWrite || ia.type == ImageAccessType::TransferWrite) && checkWrite(ia.image.id))
							{
								intermediateWrite = true;
								break;
							}
						}
					}
					if (intermediateWrite)
					{
						break;
					}
				}

				if (intermediateWrite)
				{
					unsplittable.push_back(b);
					continue;
				}

				auto& producerCp = m_compiled[producerCi];

				// One event per producer/consumer PAIR. Sharing a single event per producer
				// was the bug: the producer signalled the union of what all its consumers
				// needed while each consumer waited on its own subset, and the spec requires
				// the wait's dependency info to be exactly equal to the set's. A prepass
				// writing depth and a G-buffer, read by two different passes, is enough to
				// trip it.
				auto pairIt = producerToWait.find(producerCi);
				if (pairIt == producerToWait.end())
				{
					const std::uint32_t eventIndex = m_storage->AllocateEvent();
					if (eventIndex == UINT32_MAX)
					{
						// Out of events: fall back to an ordinary barrier rather than
						// emitting a split half that nothing can wait on.
						unsplittable.push_back(b);
						continue;
					}
					producerCp.signals.push_back(CompiledSignal{.eventIndex = eventIndex, .barriers = {}});
					cp.waits.push_back(CompiledWait{.eventIndex = eventIndex, .barriers = {}});
					pairIt = producerToWait.emplace(producerCi, eventIndex).first;
				}

				const std::uint32_t eventIndex = pairIt->second;

				// Appended to both halves together and in the same order, so the two
				// dependency infos stay element-for-element identical.
				auto sigIt = std::ranges::find_if(producerCp.signals, [eventIndex](const CompiledSignal& sg) { return sg.eventIndex == eventIndex; });
				auto waitIt = std::ranges::find_if(cp.waits, [eventIndex](const CompiledWait& w) { return w.eventIndex == eventIndex; });
				AE_ASSERT(sigIt != producerCp.signals.end() && waitIt != cp.waits.end(), "split barrier pair went missing after being inserted");
				sigIt->barriers.push_back(b);
				waitIt->barriers.push_back(b);
			}

			cp.preBarriers = std::move(unsplittable);
		}

#ifndef NDEBUG
		const auto issues = EvaluateBarriers();
		for (const auto& issue: issues)
		{
			AE_WARN(LogCategory::Vulkan, "{}", issue.message);
		}
#endif
	}

#ifndef NDEBUG
	std::vector<RenderGraph::BarrierIssue> RenderGraph::EvaluateBarriers() const
	{
		std::vector<BarrierIssue> issues;

		for (const CompiledPass& cp: m_compiled)
		{
			const PassRecord& pass = m_passes[cp.passIndex];

			for (const CompiledBarrier& b: cp.preBarriers)
			{
				if (b.oldLayout == b.newLayout && b.srcStage == b.dstStage && b.srcAccess == b.dstAccess)
				{
					issues.push_back({
					        .kind = BarrierIssue::Kind::Redundant,
					        .passIndex = cp.passIndex,
					        .resourceId = b.resourceId,
					        .message = std::format("Pass '{}': redundant barrier on resource {} -- layout and stage unchanged", pass.name, b.resourceId),
					});
				}

				if (b.oldLayout != b.newLayout && b.srcAccess == 0 && b.srcStage != static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT) && !b.isWAR)
				{
					issues.push_back({
					        .kind = BarrierIssue::Kind::MissingAccessMask,
					        .passIndex = cp.passIndex,
					        .resourceId = b.resourceId,
					        .message = std::format("Pass '{}': layout transition on resource {} has srcAccess=0 "
					                               "but srcStage is not TOP_OF_PIPE -- possible RAW hazard.",
					                pass.name,
					                b.resourceId),
					});
				}

				if (b.newLayout == gpu::ImageLayout::ShaderReadOnly && b.dstStage == static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) && pass.kind == PassKind::Graphics)
				{
					issues.push_back({
					        .kind = BarrierIssue::Kind::VertexSamplingGap,
					        .passIndex = cp.passIndex,
					        .resourceId = b.resourceId,
					        .message = std::format("Pass '{}': resource {} transitions to ShaderReadOnly "
					                               "with only FRAGMENT_SHADER stage -- vertex shader sampling will race.",
					                pass.name,
					                b.resourceId),
					});
				}
			}
		}
		return issues;
	}
#endif

	void RenderGraph::Execute(gpu::CommandList& cmdList, const FrameResourceContext& frame)
	{
		if (m_passes.empty())
		{
			return;
		}
		m_lastFrameContext = frame;

		Compile();

		if (m_diagnosticEngine != nullptr)
		{
			for (std::size_t i = 0; i < m_compiled.size(); i++)
			{
				m_diagnosticEngine->RegisterBreadcrumbLabel(static_cast<std::uint32_t>(i), m_passes[m_compiled[i].passIndex].name);
			}
		}

		const FrameTarget& target = frame.target;
		const std::uint32_t frameIndex = frame.frameSlot;

		m_storage->PrepareTransientAllocations(target, m_transientImageLifetimes, m_transientBufferLifetimes);

		m_storage->EnsureTransientImages(target);
		m_storage->EnsureTransientBuffers();
		m_storage->RefreshTransientStats();

		m_storage->GetLastFrameStats().passCount = static_cast<std::uint32_t>(m_compiled.size());

		// Begun here rather than where the compute passes are recorded: this waits the
		// compute queue's fence for this frame slot, which together with the graphics fence
		// already waited before recording is what makes the host-side timestamp reset below
		// safe. Both queues are then known to be finished with the slot's query pool.
		const bool hasAsyncCompute = HasAsyncComputeWork();
		if (hasAsyncCompute)
		{
			m_storage->BeginComputeCommandBuffer(frameIndex);
		}

		ResolveGpuTimings(frameIndex);
		ResetGpuTimings(frameIndex, static_cast<std::uint32_t>(m_compiled.size()));

		auto frameAddr = static_cast<gpu::DeviceAddress>(frame.frameConstantsAddr);

		// solver migration: the engine code never names VkImage.
		auto resolveImage = [&](uint32_t resourceId) -> gpu::Image
		{
			if (resourceId == kSwapchainColorId)
			{
				return target.colorImage;
			}
			if (resourceId == kSwapchainDepthId)
			{
				return target.depthImage;
			}
			if (IsTransientId(resourceId))
			{
				return m_storage->ResolveTransientImage(TransientIndex(resourceId));
			}
			const uint32_t extIdx = ExternalIndex(resourceId);
			return m_storage->GetExternalImage(extIdx);
		};

		auto resolveBuffer = [&](uint32_t resourceId) -> gpu::Buffer
		{
			if (IsTransientBufferId(resourceId))
			{
				return m_storage->ResolveTransientBuffer(TransientBufferIndex(resourceId));
			}
			const uint32_t extIdx = ExternalBufferIndex(resourceId);
			return m_storage->GetExternalBuffer(extIdx);
		};

		// barrier solver migration: the engine code never names VkCommandBuffer.
		auto executePassOn = [&](const CompiledPass& cp, gpu::CommandList& recorder, gpu::CommandBuffer cmd, uint32_t breadcrumbValue)
		{
			PassRecord& pass = m_passes[cp.passIndex];
			AE_PROFILE_ZONE();
			AE_PROFILE_SET_ZONE_NAME(pass.name.c_str());
			recorder.BeginDebugLabel(pass.name, 0.20f, 0.70f, 0.35f, 1.0f);
			if (m_diagnosticEngine != nullptr)
			{
				m_diagnosticEngine->RecordEvent("Begin pass {}", pass.name);
			}

			auto& scratchEventBars = m_storage->GetScratchSignalBarriers();
			for (const CompiledWait& w: cp.waits)
			{
				if (w.barriers.empty())
				{
					continue;
				}

				const gpu::Event event = m_storage->GetEvent(w.eventIndex);
				if (event == nullptr)
				{
					continue;
				}

				scratchEventBars.clear();
				for (const CompiledBarrier& b: w.barriers)
				{
					const gpu::Image image = resolveImage(b.resourceId);
					if (image == nullptr)
					{
						AE_WARN(LogCategory::Vulkan, "RenderGraph: could not resolve image id={} for split wait in pass '{}'.", b.resourceId, pass.name);
						continue;
					}

					scratchEventBars.push_back(gpu::ImageMemoryBarrier{
					        .image = image,
					        .oldLayout = b.oldLayout,
					        .newLayout = b.newLayout,
					        .aspect = b.aspect,
					        .srcStage = static_cast<gpu::PipelineStage>(b.srcStage),
					        .srcAccess = static_cast<gpu::AccessFlags>(b.srcAccess),
					        .dstStage = static_cast<gpu::PipelineStage>(b.dstStage),
					        .dstAccess = static_cast<gpu::AccessFlags>(b.dstAccess),
					});
				}

				if (!scratchEventBars.empty())
				{
					aether::RenderGraphStorage::CmdWaitEvents2(cmd, event, std::span<const gpu::ImageMemoryBarrier>(scratchEventBars));
				}
			}

			auto& scratchBarriers = m_storage->GetScratchBarriers();
			scratchBarriers.clear();
			for (const CompiledBarrier& b: cp.preBarriers)
			{
				const gpu::Image image = resolveImage(b.resourceId);

				if (image == nullptr)
				{
					AE_WARN(LogCategory::Vulkan, "RenderGraph: could not resolve image id={} for barrier in pass '{}'.", b.resourceId, pass.name);
					continue;
				}

				m_storage->GetLastFrameStats().barrierCount++;

#ifndef NDEBUG
				{
					const gpu::ImageLayout tracked = m_storage->GetTrackedLayout(image);
					if (tracked != gpu::ImageLayout::Undefined && b.oldLayout != gpu::ImageLayout::Undefined && tracked != b.oldLayout)
					{
						AE_WARN(LogCategory::Vulkan,
						        "RenderGraph: layout mismatch on image id={} in pass '{}': "
						        "compiled oldLayout={} but oracle tracks {}.",
						        b.resourceId,
						        pass.name,
						        static_cast<int>(b.oldLayout),
						        static_cast<int>(tracked));
					}
				}
				m_storage->SetTrackedLayout(image, b.newLayout);
#endif

				scratchBarriers.push_back(gpu::ImageMemoryBarrier{
				        .image = image,
				        .oldLayout = b.oldLayout,
				        .newLayout = b.newLayout,
				        .aspect = b.aspect,
				        .srcStage = static_cast<gpu::PipelineStage>(b.srcStage),
				        .srcAccess = static_cast<gpu::AccessFlags>(b.srcAccess),
				        .dstStage = static_cast<gpu::PipelineStage>(b.dstStage),
				        .dstAccess = static_cast<gpu::AccessFlags>(b.dstAccess),
				});
			}
			aether::RenderGraphStorage::CmdImageBarriers(cmd, std::span<const gpu::ImageMemoryBarrier>(scratchBarriers));

			auto& scratchBufBars = m_storage->GetScratchBufferBarriers();
			scratchBufBars.clear();
			for (const CompiledBufferBarrier& b: cp.bufferBarriers)
			{
				const gpu::Buffer buffer = resolveBuffer(b.resourceId);
				if (buffer == nullptr)
				{
					continue;
				}

				scratchBufBars.push_back(gpu::BufferMemoryBarrier{
				        .buffer = buffer,
				        .offset = 0,
				        .size = static_cast<gpu::DeviceSize>(-1),
				        .srcStage = static_cast<gpu::PipelineStage>(b.srcStage),
				        .srcAccess = static_cast<gpu::AccessFlags>(b.srcAccess),
				        .dstStage = static_cast<gpu::PipelineStage>(b.dstStage),
				        .dstAccess = static_cast<gpu::AccessFlags>(b.dstAccess),
				});
			}
			aether::RenderGraphStorage::CmdBufferBarriers(cmd, std::span<const gpu::BufferMemoryBarrier>(scratchBufBars));

			auto& scratchColorInfos = m_storage->GetScratchColorInfos();
			scratchColorInfos.clear();
			for (const AttachmentRef& a: pass.colorWrites)
			{
				gpu::ImageView view = nullptr;
				if (a.image.id == kSwapchainColorId)
				{
					view = target.colorView;
				}
				else if (IsTransientId(a.image.id))
				{
					view = m_storage->ResolveTransientView(TransientIndex(a.image.id));
				}
				else
				{
					view = m_storage->GetExternalView(ExternalIndex(a.image.id));
				}

				scratchColorInfos.push_back(gpu::RenderingAttachmentInfo{
				        .imageView = view,
				        .imageLayout = gpu::ImageLayout::ColorAttachment,
				        .loadOp = a.loadOp,
				        .storeOp = a.storeOp,
				        .clearValue = a.clearValue,
				});
			}

			std::optional<gpu::RenderingAttachmentInfo> depthInfo;
			if (pass.depthWrite.has_value())
			{
				const AttachmentRef& da = *pass.depthWrite;
				gpu::ImageView depthView = nullptr;
				if (da.image.id == kSwapchainDepthId)
				{
					depthView = target.depthView;
				}
				else if (IsTransientId(da.image.id))
				{
					depthView = m_storage->ResolveTransientView(TransientIndex(da.image.id));
				}
				else
				{
					depthView = m_storage->GetExternalView(ExternalIndex(da.image.id));
				}

				depthInfo = gpu::RenderingAttachmentInfo{
				        .imageView = depthView,
				        .imageLayout = gpu::ImageLayout::DepthAttachment,
				        .loadOp = da.loadOp,
				        .storeOp = da.storeOp,
				        .clearValue = da.clearValue,
				};
			}

			const gpu::Extent2D passExtent = pass.extentOverride.value_or(target.extent);
			bool passDebugDisabled = false;
			{
				const std::scoped_lock lock(m_debugStateMutex);
				passDebugDisabled = pass.debugDisabled;
			}
			const bool hasAttachments = pass.kind == PassKind::Graphics && (!scratchColorInfos.empty() || depthInfo.has_value());
			// legally begin dynamic rendering (renderArea and viewport must be > 0).
			const bool degenerateExtent = hasAttachments && (passExtent.width == 0 || passExtent.height == 0);
			const bool skipPassBody = passDebugDisabled || degenerateExtent;
			const bool useDynamicRendering = !skipPassBody && hasAttachments;
			if (useDynamicRendering)
			{
				const gpu::RenderingInfo renderInfo{
				        .width = passExtent.width,
				        .height = passExtent.height,
				        .layerCount = 1,
				        .colorAttachments = std::span<const gpu::RenderingAttachmentInfo>(scratchColorInfos),
				        .depthAttachment = depthInfo.has_value() ? &depthInfo.value() : nullptr,
				};
				recorder.BeginRendering(renderInfo);

				const gpu::Viewport viewport{
				        .x = 0.0f,
				        .y = 0.0f,
				        .width = static_cast<float>(passExtent.width),
				        .height = static_cast<float>(passExtent.height),
				        .minDepth = 0.0f,
				        .maxDepth = 1.0f,
				};
				const gpu::Rect2D scissor{
				        .x = 0,
				        .y = 0,
				        .width = passExtent.width,
				        .height = passExtent.height,
				};
				recorder.SetViewport(viewport);
				recorder.SetScissor(scissor);
			}

			if (m_diagnosticEngine != nullptr)
			{
				m_diagnosticEngine->WriteBreadcrumb(reinterpret_cast<VkCommandBuffer>(cmd), breadcrumbValue);
			}

			if (skipPassBody)
			{
				if (pass.debugDisabledExecute)
				{
					PassContext ctx{.recorder = recorder, .graph = *this, .frame = frame, .extent = passExtent, .frameConstantsAddr = frameAddr, .frameIndex = frameIndex, .frameSlot = frame.frameSlot};
					pass.debugDisabledExecute(ctx);
				}
				const std::scoped_lock lock(m_debugStateMutex);
				pass.lastCpuTimeMs = 0.0f;
				pass.lastGpuTimeMs = 0.0f;
			}
			else if (pass.execute)
			{
				AE_GPU_ZONE_SCOPED(cmd, pass.name);

				// AllCommands both sides: the pair brackets everything this pass submits,
				// which is what "how long did this pass cost the GPU" means at graph level.
				GpuTimingFrame& timing = m_gpuTiming[frameIndex];
				const bool timed = timing.pool != nullptr && (timing.used + 2u) <= timing.capacity;
				if (timed)
				{
					recorder.WriteTimestamp(timing.pool, timing.used, gpu::PipelineStage::AllCommands);
				}

				const auto t0 = std::chrono::high_resolution_clock::now();
				PassContext ctx{.recorder = recorder, .graph = *this, .frame = frame, .extent = passExtent, .frameConstantsAddr = frameAddr, .frameIndex = frameIndex, .frameSlot = frame.frameSlot};
				pass.execute(ctx);
				const auto t1 = std::chrono::high_resolution_clock::now();

				if (timed)
				{
					recorder.WriteTimestamp(timing.pool, timing.used + 1u, gpu::PipelineStage::AllCommands);
					timing.timedPasses.push_back(cp.passIndex);
					timing.used += 2u;
					timing.pending = true;
				}

				const std::scoped_lock lock(m_debugStateMutex);
				pass.lastCpuTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
			}

			if (m_diagnosticEngine != nullptr)
			{
				m_diagnosticEngine->RecordEvent("End pass {}", pass.name);
			}

			if (useDynamicRendering)
			{
				recorder.EndRendering();
			}

			for (const CompiledSignal& sg: cp.signals)
			{
				if (sg.barriers.empty())
				{
					continue;
				}
				const gpu::Event event = m_storage->GetEvent(sg.eventIndex);
				if (event != nullptr)
				{
					scratchEventBars.clear();
					for (const CompiledBarrier& b: sg.barriers)
					{
						const gpu::Image image = resolveImage(b.resourceId);
						if (image == nullptr)
						{
							AE_WARN(LogCategory::Vulkan, "RenderGraph: could not resolve image id={} for split signal in pass '{}'.", b.resourceId, pass.name);
							continue;
						}

						m_storage->GetLastFrameStats().barrierCount++;

#ifndef NDEBUG
						m_storage->SetTrackedLayout(image, b.newLayout);
#endif

						scratchEventBars.push_back(gpu::ImageMemoryBarrier{
						        .image = image,
						        .oldLayout = b.oldLayout,
						        .newLayout = b.newLayout,
						        .aspect = b.aspect,
						        .srcStage = static_cast<gpu::PipelineStage>(b.srcStage),
						        .srcAccess = static_cast<gpu::AccessFlags>(b.srcAccess),
						        .dstStage = static_cast<gpu::PipelineStage>(b.dstStage),
						        .dstAccess = static_cast<gpu::AccessFlags>(b.dstAccess),
						});
					}

					if (!scratchEventBars.empty())
					{
						aether::RenderGraphStorage::CmdSetEvent2(cmd, event, std::span<const gpu::ImageMemoryBarrier>(scratchEventBars));
					}
				}
			}

			recorder.EndDebugLabel();
		};

		std::uint32_t passMarker = 0;

		if (hasAsyncCompute)
		{
			const gpu::CommandBuffer computeCmd = m_storage->GetComputeCommandBuffer(frameIndex);
			gpu::CommandList computeRecorder(computeCmd);
			computeRecorder.BeginDebugLabel("Frame.RenderGraph.AsyncCompute", 0.90f, 0.45f, 0.10f, 1.0f);

			for (const CompiledPass& cp: m_compiled)
			{
				if (cp.queueClass != QueueClass::AsyncCompute)
				{
					break;
				}
				executePassOn(cp, computeRecorder, computeCmd, passMarker);
				passMarker++;
			}

			computeRecorder.EndDebugLabel();
			m_storage->EndComputeCommandBuffer(frameIndex);
		}

		{
			gpu::CommandList& gfxRecorder = cmdList;
			const gpu::CommandBuffer gfxCmd = cmdList.GetCommandBuffer();
			gfxRecorder.BeginDebugLabel("Frame.RenderGraph.Graphics", 0.35f, 0.55f, 0.95f, 1.0f);

			for (const CompiledPass& cp: m_compiled)
			{
				if (cp.queueClass == QueueClass::AsyncCompute)
				{
					continue;
				}
				executePassOn(cp, gfxRecorder, gfxCmd, passMarker);
				passMarker++;
			}

			gfxRecorder.EndDebugLabel();
		}

		const auto& stats = m_storage->GetLastFrameStats();
		AE_VERBOSE(LogCategory::Render,
		        "Frame {}: heap {:.1f}/{:.1f} MB, {} aliased images, {} aliased buffers, "
		        "{} cached, {} cache total",
		        frameIndex,
		        static_cast<double>(stats.heapUsed) / (1024.0 * 1024.0),
		        static_cast<double>(stats.heapCapacity) / (1024.0 * 1024.0),
		        stats.aliasedImageCount,
		        stats.aliasedBufferCount,
		        stats.transientCacheHit,
		        stats.cacheSize);
		if (m_diagnosticEngine != nullptr)
		{
			m_diagnosticEngine->EndFrame(frameIndex);
		}
	}
} // namespace aether
