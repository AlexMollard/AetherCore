#include "rendering/RenderGraph.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <numeric>
#include <queue>

#include "gpu/BindlessManager.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "gpu/GpuProfiler.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/RenderGraphStorage.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "vulkan/DiagnosticEngine.hpp"

namespace aether
{
	// -- Lifecycle ------------------------------------------------------------

	RenderGraph::RenderGraph()
	      : m_storage(std::make_unique<RenderGraphStorage>())
	{
	}

	RenderGraph::~RenderGraph() = default;

	RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
	RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

	void RenderGraph::Initialize(gpu::Device device, gpu::Allocator allocator)
	{
		m_storage->Initialize(device, allocator);
	}

	void RenderGraph::SetVulkanContext(class VulkanContext* ctx)
	{
		m_storage->SetVulkanContext(ctx);
	}

	void RenderGraph::Shutdown()
	{
		m_storage->Shutdown();
		m_externalImages.clear();
		m_passes.clear();
		m_compiled.clear();
		m_lastImageStates.clear();
		m_compileDirty = true;
	}

	void RenderGraph::BeginFrame(std::uint32_t frameIndex)
	{
		m_frameIndex = frameIndex;
		if (m_diagnosticEngine != nullptr)
		{
			m_diagnosticEngine->BeginFrame(frameIndex);
		}
		m_storage->BeginFrame(frameIndex);
	}

	// -- PassBuilder ----------------------------------------------------------

	RenderGraph::PassBuilder::PassBuilder(RenderGraph& graph, std::size_t passIndex)
	      : m_graph(graph), m_passIndex(passIndex)
	{
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteColor(RGImage image, gpu::LoadOp loadOp, gpu::StoreOp storeOp, gpu::ClearValue clearValue)
	{
		m_graph.m_passes[m_passIndex].colorWrites.push_back(AttachmentRef{
		        .image = image,
		        .loadOp = loadOp,
		        .storeOp = storeOp,
		        .clearValue = clearValue,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteDepth(RGImage image, gpu::LoadOp loadOp, gpu::StoreOp storeOp, gpu::ClearValue clearValue)
	{
		m_graph.m_passes[m_passIndex].depthWrite = AttachmentRef{
		        .image = image,
		        .loadOp = loadOp,
		        .storeOp = storeOp,
		        .clearValue = clearValue,
		};
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadTexture(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::SampledRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadStorageImage(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteStorageImage(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadBuffer(RGBuffer buffer)
	{
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::StorageRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteBuffer(RGBuffer buffer)
	{
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::StorageWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWriteBuffer(RGBuffer buffer)
	{
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::StorageReadWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::Execute(std::function<void(PassContext&)> fn)
	{
		m_graph.m_passes[m_passIndex].execute = std::move(fn);
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ExecuteCompute(std::function<void(PassContext&)> fn)
	{
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
		m_graph.m_passes[m_passIndex].execute = std::move(fn);
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::SetExtent(gpu::Extent2D extent)
	{
		m_graph.m_passes[m_passIndex].extentOverride = gpu::Extent2D{extent.width, extent.height};
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::SetQueueClass(QueueClass qc)
	{
		m_graph.m_passes[m_passIndex].queueClass = qc;
		return *this;
	}

	// -- Pass management ------------------------------------------------------

	RenderGraph::PassBuilder RenderGraph::AddPass(std::string name, [[maybe_unused]] std::source_location loc)
	{
		PassRecord rec{};
		rec.name = std::move(name);
#ifndef NDEBUG
		rec.declaredAt = loc;
#endif
		m_passes.push_back(std::move(rec));
		m_compileDirty = true;
		return PassBuilder{*this, m_passes.size() - 1};
	}

	RenderGraph::PassBuilder RenderGraph::AddComputePass(std::string name, [[maybe_unused]] std::source_location loc)
	{
		PassRecord rec{};
		rec.name = std::move(name);
		rec.kind = PassKind::Compute;
#ifndef NDEBUG
		rec.declaredAt = loc;
#endif
		m_passes.push_back(std::move(rec));
		m_compileDirty = true;
		return PassBuilder{*this, m_passes.size() - 1};
	}

	const FrameStats& RenderGraph::GetFrameStats() const
	{
		return m_storage->GetLastFrameStats();
	}

	void RenderGraph::RemovePass(const std::string& name)
	{
		const auto it = std::ranges::find_if(m_passes, [&](const PassRecord& p) { return p.name == name; });
		if (it != m_passes.end())
		{
			m_passes.erase(it);
			m_compileDirty = true;
		}
	}

	void RenderGraph::Clear()
	{
		for (auto& [id, state]: m_lastImageStates)
		{
			if (IsTransientId(id))
			{
				const uint32_t idx = TransientIndex(id);
				if (idx < m_storage->GetTransientCount())
				{
					m_storage->ReleaseTransient(idx);
				}
			}
		}
		m_storage->ClearExternalImages();
		m_externalImages.clear();
		m_passes.clear();
		m_compiled.clear();
		m_lastImageStates.clear();
		m_compileDirty = true;
	}

	std::vector<RenderGraph::PassInfo> RenderGraph::GetPasses() const
	{
		std::vector<PassInfo> result;
		result.reserve(m_passes.size());
		for (const auto& pass: m_passes)
		{
			result.push_back(PassInfo{
			        .name = pass.name,
			        .isGraphics = pass.kind == PassKind::Graphics,
			        .isCompute = pass.kind == PassKind::Compute,
			        .isAsyncCompute = pass.queueClass == QueueClass::AsyncCompute,
			        .lastCpuTimeMs = pass.lastCpuTimeMs,
			});
		}
		return result;
	}

	void RenderGraph::EnableAsyncCompute(gpu::Queue computeQueue, std::uint32_t computeQueueFamily)
	{
		m_storage->EnableAsyncCompute(computeQueue, computeQueueFamily);
		m_asyncComputeEnabled = true;
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

	// -- Image registration ---------------------------------------------------

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
		// Reset buffer state - the backing buffer has been replaced, so the
		// previous frame's barrier tracking is stale. The next Compile() will
		// emit a fresh TOP_OF_PIPE barrier for this buffer.
		m_lastBufferStates.erase(buffer.id);
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

	// -- Bindless -------------------------------------------------------------

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

	// -- Compilation ----------------------------------------------------------

	void RenderGraph::Compile()
	{
		if (!m_compileDirty)
		{
			return;
		}
		m_compileDirty = false;

		m_storage->ResetEvents();

		const std::size_t N = m_passes.size();
		m_compiled.clear();
		m_compiled.reserve(N);

		if (m_asyncComputeEnabled)
		{
			for (auto& pass: m_passes)
			{
				if (pass.kind == PassKind::Compute && pass.queueClass == QueueClass::Graphics && pass.colorWrites.empty() && !pass.depthWrite.has_value() && pass.imageAccesses.empty())
				{
					pass.queueClass = QueueClass::AsyncCompute;
				}
			}
		}

		std::vector<std::vector<std::size_t>> adj(N);
		std::vector<std::size_t> inDegree(N, 0);

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
				if (a.image.id == resId && a.type == ImageAccessType::StorageWrite)
				{
					return true;
				}
			}
			for (const BufferAccessRef& a: m_passes[idx].bufferAccesses)
			{
				if (a.buffer.id == resId && a.type == BufferAccessType::StorageWrite)
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
						if (ia.type == ImageAccessType::StorageWrite && passAccesses(j, ia.image.id))
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
						if (ba.type == BufferAccessType::StorageWrite && passAccesses(j, ba.buffer.id))
						{
							dependent = true;
							break;
						}
					}
				}
				if (dependent)
				{
					adj[i].push_back(j);
					++inDegree[j];
				}
			}
		}

		struct ReadyCompare
		{
			const std::vector<PassRecord>& passes;
			const std::vector<std::vector<std::size_t>>& adj;

			[[nodiscard]] bool operator()(std::size_t a, std::size_t b) const
			{
				// Async compute passes have highest priority - run all AC work
				// before any graphics work to maximize GPU queue overlap.
				const bool aAC = passes[a].queueClass == QueueClass::AsyncCompute;
				const bool bAC = passes[b].queueClass == QueueClass::AsyncCompute;
				if (aAC != bAC)
				{
					return bAC; // true → b has higher priority
				}

				const auto aConsumers = adj[a].size();
				const auto bConsumers = adj[b].size();
				if (aConsumers != bConsumers)
				{
					return aConsumers < bConsumers; // true → b has more consumers
				}

				// Tiebreaker: declaration order (deterministic).
				return a > b;
			}
		};

		ReadyCompare readyCmp{.passes = m_passes, .adj = adj};
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
			// Find first pass involved in the cycle for diagnostic output.
			// sortedIndices contains whatever made it through; the first gap
			// or the first unsorted pass are good candidates to report.
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

		// -- Dead Store Elimination -----------------------------------------------
		// A pass is dead if none of its image outputs are read by any later pass,
		// and it has no side effects (swapchain writes, external image writes).
		// Culled passes are skipped entirely - no barriers are compiled for them,
		// and Execute() never dispatches their work.

		std::vector<bool> passCulledByPassIdx(N, false);
		{
			// Build map: resourceId → sorted positions of passes that read it
			std::unordered_map<uint32_t, std::vector<std::size_t>> resourceReaders;
			for (std::size_t i = 0; i < N; ++i)
			{
				const std::size_t passIdx = sortedIndices[i];
				const PassRecord& pass = m_passes[passIdx];

				auto recordRead = [&](uint32_t resId)
				{
					resourceReaders[resId].push_back(i);
				};

				for (const ImageAccessRef& r: pass.imageAccesses)
				{
					if (r.type != ImageAccessType::StorageWrite)
					{
						recordRead(r.image.id);
					}
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

				// Collect write targets
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
					if (ia.type == ImageAccessType::StorageWrite)
					{
						writeTargets.push_back(ia.image.id);
					}
				}
				for (const BufferAccessRef& ba: pass.bufferAccesses)
				{
					if (ba.type == BufferAccessType::StorageWrite)
					{
						writeTargets.push_back(ba.buffer.id);
					}
				}

				// No tracked image writes → can't prove no side effects (buffers, external state)
				if (writeTargets.empty())
				{
					continue;
				}

				// Side-effect targets are never dead
				auto isSideEffect = [&](uint32_t resId) -> bool
				{
					return resId == kSwapchainColorId || resId == kSwapchainDepthId || (resId >= kFirstExternalId && resId < kFirstTransientId);
				};

				bool hasSideEffect = false;
				for (uint32_t resId: writeTargets)
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

				// Check if any write is read by a later pass
				bool anyReaderFound = false;
				for (uint32_t resId: writeTargets)
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
#ifndef NDEBUG
					std::string deadResources;
					for (uint32_t resId: writeTargets)
					{
						if (!deadResources.empty())
						{
							deadResources += ", ";
						}
						deadResources += std::to_string(resId);
					}
					AE_WARN(LogCategory::Engine, "Pass '{}' writes to RGImage(s) {}, but no subsequent pass reads {}. Culled from execution.", pass.name, deadResources, writeTargets.size() > 1 ? "them" : "it");
#endif
				}
			}
		}

		// Validate queue grouping: all async-compute passes must come before
		// all graphics passes in topological order. Interleaving would require
		// multiple submissions per queue per frame, which we intentionally avoid.
		// The priority-queue topological sort already maximizes AC grouping;
		// this pass only demotes AC passes that were interleaved due to
		// unavoidable dependency ordering.
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
					// Demote to graphics queue to maintain correctness.
					m_passes[idx].queueClass = QueueClass::Graphics;
				}
			}
		}

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

		// Restore buffer state from the previous frame.
		std::unordered_map<uint32_t, BufferState> bufferStates;
		for (const auto& [id, s]: m_lastBufferStates)
		{
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
				constexpr gpu::ImageLayout kTarget = gpu::ImageLayout::ColorAttachment;
				constexpr auto kDstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
				constexpr auto kDstWrite = static_cast<std::uint64_t>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
				constexpr auto kDstReadWrite = static_cast<std::uint64_t>(VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

				const auto it = states.find(resId);
				if (it != states.end())
				{
					const ResourceState& s = it->second;
					const bool layoutChange = (s.layout != kTarget);
					const bool loadRead = (a.loadOp == gpu::LoadOp::Load);
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
					        .srcStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT),
					        .srcAccess = 0,
					        .dstStage = kDstStage,
					        .dstAccess = kDstWrite,
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
					        .srcStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT),
					        .srcAccess = 0,
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
				}

				const bool isRead = (r.type != ImageAccessType::StorageWrite);

				auto it = states.find(resId);

				// RAR: already in the right layout and only reads since last write -> no barrier
				if (isRead && it != states.end() && it->second.layout == targetLayout && it->second.writeStage == 0)
				{
					it->second.readStages |= dstStage;
					continue;
				}

				std::uint64_t srcStage;
				std::uint64_t srcAccess;
				gpu::ImageLayout oldLayout;
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
					srcStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
					srcAccess = 0;
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

			// -- Buffer barrier compilation ---------------------------------
			for (const BufferAccessRef& r: pass.bufferAccesses)
			{
				const uint32_t resId = r.buffer.id;

				const bool isComputePass = (pass.kind == PassKind::Compute);
				constexpr auto kComputeStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				constexpr auto kGraphicsStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
				constexpr auto kStorageRead = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
				constexpr auto kStorageWrite = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

				const std::uint64_t dstStage = isComputePass ? kComputeStage : kGraphicsStage;
				const bool isRead = (r.type == BufferAccessType::StorageRead);
				const bool isReadWrite = (r.type == BufferAccessType::StorageReadWrite);
				const std::uint64_t dstAccess = isRead ? kStorageRead : kStorageWrite;

				auto it = bufferStates.find(resId);

				// RAR: already in a read-only state → no barrier
				if (isRead && it != bufferStates.end() && it->second.writeStage == 0)
				{
					it->second.readStages |= dstStage;
					continue;
				}

				std::uint64_t srcStage;
				std::uint64_t srcAccess;
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
					srcStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
					srcAccess = 0;
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

		// -- Split barrier post-processing ---------------------------------
		// Build a map: resource -> last compiled-pass index that wrote it.
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
				if (ia.type == ImageAccessType::StorageWrite)
				{
					recordWrite(ia.image.id);
				}
			}
		}

		// Eligible barrier check: producers must be ≥2 apart, no intermediate
		// writer, and not cross-frame / WAR.
		for (std::size_t ci = 0; ci < m_compiled.size(); ++ci)
		{
			auto& cp = m_compiled[ci];

			std::vector<CompiledBarrier> unsplittable;
			unsplittable.reserve(cp.preBarriers.size());

			for (const CompiledBarrier& b: cp.preBarriers)
			{
				// WAR barriers cannot be split.
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

				// Split barriers (VkEvent) cannot cross queue boundaries.
				// Timeline semaphores handle cross-queue sync at submission level.
				if (m_compiled[producerCi].queueClass != m_compiled[ci].queueClass)
				{
					unsplittable.push_back(b);
					continue;
				}

				// Adjacent passes gain nothing from the event overhead.
				if (producerCi + 1 >= ci)
				{
					unsplittable.push_back(b);
					continue;
				}

				// Ensure no intermediate pass writes the same resource.
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
							if (ia.type == ImageAccessType::StorageWrite && checkWrite(ia.image.id))
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

				// Barrier is eligible for splitting.
				auto& producerCp = m_compiled[producerCi];
				if (producerCp.splitEventIndex == UINT32_MAX)
				{
					producerCp.splitEventIndex = m_storage->AllocateEvent();
				}

				producerCp.signalBarriers.push_back(b);

				// Group waits by event to minimise cmdWaitEvents2 calls.
				auto waitIt = std::ranges::find_if(cp.waits, [eventIdx = producerCp.splitEventIndex](const CompiledWait& w) { return w.eventIndex == eventIdx; });

				if (waitIt != cp.waits.end())
				{
					waitIt->barriers.push_back(b);
				}
				else
				{
					cp.waits.push_back(CompiledWait{
					        .eventIndex = producerCp.splitEventIndex,
					        .barriers = {b},
					});
				}
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

	// -- Execution ------------------------------------------------------------

	void RenderGraph::Execute(gpu::CommandList& cmdList, const FrameTarget& target, std::uint64_t frameConstantsAddr, std::uint32_t frameIndex)
	{
		if (m_passes.empty())
		{
			return;
		}

		Compile();

		// Register breadcrumb labels for all compiled passes so the
		// diagnostic engine can resolve marker values to pass names.
		if (m_diagnosticEngine != nullptr)
		{
			for (std::size_t i = 0; i < m_compiled.size(); i++)
			{
				m_diagnosticEngine->RegisterBreadcrumbLabel(static_cast<std::uint32_t>(i), m_passes[m_compiled[i].passIndex].name);
			}
		}

		// Two-pass transient heap preparation: query memory requirements, allocate heap, assign virtual offsets.
		m_storage->PrepareTransientAllocations(target);

		// Allocate aliased VkImage/VkBuffer handles from the pre-computed heap offsets.
		m_storage->EnsureTransientImages(target);
		m_storage->EnsureTransientBuffers();

		m_storage->GetLastFrameStats().passCount = static_cast<std::uint32_t>(m_compiled.size());

		auto frameAddr = static_cast<gpu::DeviceAddress>(frameConstantsAddr);

		// Image resolution helper shared by pre-, wait-, and signal-barriers.
		// Returns the opaque gpu::Image (the actual VkImage is obtained
		// in the storage via static_cast at emit time). P5(d) barrier
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
			const uint32_t extIdx = ExternalBufferIndex(resourceId);
			return m_storage->GetExternalBuffer(extIdx);
		};

		// Execute a single compiled pass on the given command list.
		// cmd is the opaque gpu::CommandBuffer (the actual VkCommandBuffer
		// is obtained in the storage via static_cast at emit time). P5(d)
		// barrier solver migration: the engine code never names VkCommandBuffer.
		auto executePassOn = [&](const CompiledPass& cp, gpu::CommandList& recorder, gpu::CommandBuffer cmd, uint32_t breadcrumbValue)
		{
			PassRecord& pass = m_passes[cp.passIndex];
			AE_PROFILE_ZONE_N("RenderPass");
			AE_PROFILE_SET_ZONE_NAME(pass.name.c_str());
			recorder.BeginDebugLabel(pass.name, 0.20f, 0.70f, 0.35f, 1.0f);
			if (m_diagnosticEngine != nullptr)
			{
				m_diagnosticEngine->RecordEvent("Begin pass {}", pass.name);
			}

			// -- Split barrier waits (consume events from producers) -----------
			// P5(d) barrier solver migration: all barriers are engine-side
			// structs (gpu::ImageMemoryBarrier / gpu::BufferMemoryBarrier).
			// The storage translates to Vk* and calls cmd*Event2.
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
					m_storage->CmdWaitEvents2(cmd, event, std::span<const gpu::ImageMemoryBarrier>(scratchEventBars));
				}
			}

			// -- Barriers (unsplittable) ------------------------------------
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
			m_storage->CmdImageBarriers(cmd, std::span<const gpu::ImageMemoryBarrier>(scratchBarriers));

			// -- Buffer barriers --------------------------------------------
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
				        .size = static_cast<gpu::DeviceSize>(-1), // VK_WHOLE_SIZE
				        .srcStage = static_cast<gpu::PipelineStage>(b.srcStage),
				        .srcAccess = static_cast<gpu::AccessFlags>(b.srcAccess),
				        .dstStage = static_cast<gpu::PipelineStage>(b.dstStage),
				        .dstAccess = static_cast<gpu::AccessFlags>(b.dstAccess),
				});
			}
			m_storage->CmdBufferBarriers(cmd, std::span<const gpu::BufferMemoryBarrier>(scratchBufBars));

			// -- Dynamic rendering -------------------------------------------
			// P5(d) barrier solver migration: build engine-side
			// gpu::RenderingAttachmentInfo + gpu::RenderingInfo. The storage
			// translates to backend types.
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
			const bool useDynamicRendering = pass.kind == PassKind::Graphics && (!scratchColorInfos.empty() || depthInfo.has_value());
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

			if (pass.execute)
			{
				// Tracy GPU zone. The engine-side macro captures
				// __FILE__/__LINE__ at this call site; the cast and
				// Tracy plumbing live in vulkan/GpuProfiler.cpp.
				AE_GPU_ZONE_SCOPED(cmd, pass.name);
				const auto t0 = std::chrono::high_resolution_clock::now();
				PassContext ctx{.recorder = recorder, .extent = passExtent, .frameConstantsAddr = frameAddr, .frameIndex = frameIndex};
				pass.execute(ctx);
				const auto t1 = std::chrono::high_resolution_clock::now();
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

			// -- Split barrier signals (set events for later consumers) --------
			if (!cp.signalBarriers.empty())
			{
				const gpu::Event event = m_storage->GetEvent(cp.splitEventIndex);
				if (event != nullptr)
				{
					scratchEventBars.clear();
					for (const CompiledBarrier& b: cp.signalBarriers)
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
						        .dstStage = static_cast<gpu::PipelineStage>(b.dstStage), // ignored by set
						        .dstAccess = static_cast<gpu::AccessFlags>(b.dstAccess), // ignored by set
						});
					}

					if (!scratchEventBars.empty())
					{
						m_storage->CmdSetEvent2(cmd, event, std::span<const gpu::ImageMemoryBarrier>(scratchEventBars));
					}
				}
			}

			recorder.EndDebugLabel();
		};

		const bool hasAsyncCompute = HasAsyncComputeWork();
		std::uint32_t passMarker = 0;

		// Phase 1: Execute async-compute passes on the dedicated compute queue.
		if (hasAsyncCompute)
		{
			m_storage->BeginComputeCommandBuffer(frameIndex);
			const gpu::CommandBuffer computeCmd = m_storage->GetComputeCommandBuffer(frameIndex);
			gpu::CommandList computeRecorder(computeCmd);
			computeRecorder.BeginDebugLabel("Frame.RenderGraph.AsyncCompute", 0.90f, 0.45f, 0.10f, 1.0f);

			for (const CompiledPass& cp: m_compiled)
			{
				if (cp.queueClass != QueueClass::AsyncCompute)
				{
					break; // validated: all async-compute passes precede graphics
				}
				executePassOn(cp, computeRecorder, computeCmd, passMarker);
				passMarker++;
			}

			computeRecorder.EndDebugLabel();
			m_storage->EndComputeCommandBuffer(frameIndex);
			// Compute queue submission is deferred to SubmitComputeWork(),
			// called by the frame orchestrator right before graphics submission
			// so both queues are submitted back-to-back for maximum GPU overlap.
		}

		// Phase 2: Execute graphics passes on the main graphics command list.
		{
			gpu::CommandList& gfxRecorder = cmdList;
			// P5(d) barrier solver migration: pass the opaque
			// gpu::CommandBuffer to executePassOn; the storage casts to
			// VkCommandBuffer at emit time. The raw VkCommandBuffer is
			// only needed for Tracy's GPU collection (audit §7.3.0
			// allowlist exception).
			const gpu::CommandBuffer gfxCmd = cmdList.GetCommandBuffer();
			gfxRecorder.BeginDebugLabel("Frame.RenderGraph.Graphics", 0.35f, 0.55f, 0.95f, 1.0f);

			bool foundGraphics = false;
			for (const CompiledPass& cp: m_compiled)
			{
				if (cp.queueClass == QueueClass::AsyncCompute)
				{
					continue; // skip async-compute passes (already executed)
				}
				foundGraphics = true;
				executePassOn(cp, gfxRecorder, gfxCmd, passMarker);
				passMarker++;
			}

			gfxRecorder.EndDebugLabel();

			// Tracy GPU collection only from the graphics command buffer
			// since it's the one that gets submitted via SubmitAndPresent.
			if (foundGraphics)
			{
				gpu::GpuProfiler::Get().Collect(gfxCmd);
			}
		}

		// Transient heap trace logging.
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
