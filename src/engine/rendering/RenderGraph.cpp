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
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/RenderGraphStorage.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether
{
	// ── Lifecycle ────────────────────────────────────────────────────────────

	RenderGraph::RenderGraph()
	      : m_storage(std::make_unique<RenderGraphStorage>())
	{
	}

	RenderGraph::~RenderGraph() = default;

	RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
	RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

	void RenderGraph::Initialize(void* device, void* allocator)
	{
		m_storage->Initialize(static_cast<VkDevice>(device), static_cast<VmaAllocator>(allocator));
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
		m_storage->BeginFrame(frameIndex);
	}

	// ── PassBuilder ──────────────────────────────────────────────────────────

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

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadTextureCompute(RGImage image)
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

	// ── Pass management ──────────────────────────────────────────────────────

	RenderGraph::PassBuilder RenderGraph::AddPass(std::string name)
	{
		m_passes.push_back(PassRecord{.name = std::move(name)});
		m_compileDirty = true;
		return PassBuilder{*this, m_passes.size() - 1};
	}

	RenderGraph::PassBuilder RenderGraph::AddComputePass(std::string name)
	{
		m_passes.push_back(PassRecord{
		        .name = std::move(name),
		        .kind = PassKind::Compute,
		});
		m_compileDirty = true;
		return PassBuilder{*this, m_passes.size() - 1};
	}

	void RenderGraph::RemovePass(const std::string& name)
	{
		const auto it = std::find_if(m_passes.begin(), m_passes.end(), [&](const PassRecord& p) { return p.name == name; });
		if (it != m_passes.end())
		{
			m_passes.erase(it);
			m_compileDirty = true;
		}
	}

	bool RenderGraph::HasPass(std::string_view name) const
	{
		return std::find_if(m_passes.begin(), m_passes.end(), [&](const PassRecord& p) { return p.name == name; }) != m_passes.end();
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
					m_storage->ReleaseTransient(idx, m_frameIndex);
				}
			}
		}
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
			        .lastCpuTimeMs = pass.lastCpuTimeMs,
			});
		}
		return result;
	}

	// ── Image registration ───────────────────────────────────────────────────

	RGImage RenderGraph::RegisterImage(void* image, void* view, gpu::ImageAspect aspect)
	{
		const uint32_t id = kFirstExternalId + static_cast<uint32_t>(m_externalImages.size());
		m_externalImages.push_back(ExternalImageEntry{
		        .image = image,
		        .view = view,
		        .aspect = aspect,
		});
		m_storage->RegisterExternalImage(static_cast<VkImage>(image), static_cast<VkImageView>(view), gpu::ToVk(aspect));
		return RGImage{id};
	}

	RGImage RenderGraph::CreateTransientImage(const TransientImageDesc& desc)
	{
		const VkFormat vkFormat = gpu::ToVk(desc.format);
		const VkImageUsageFlags vkUsage = gpu::ToVk(desc.usage);
		const VkImageAspectFlags vkAspect = gpu::ToVk(desc.aspect);

		const uint32_t idx = m_storage->AddTransientSlot(vkFormat, vkUsage, vkAspect, desc.extent);
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

	// ── Bindless ─────────────────────────────────────────────────────────────

	std::uint32_t RenderGraph::EnsureBindlessSampled(RGImage image, BindlessManager& bindlessManager, void* device, gpu::ImageLayout descriptorLayout)
	{
		if (!IsTransientId(image.id))
		{
			return 0xFFFFFFFFu;
		}

		const uint32_t idx = TransientIndex(image.id);
		return m_storage->EnsureBindlessSampled(idx, bindlessManager, static_cast<VkDevice>(device), gpu::ToVk(descriptorLayout));
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
			m_storage->ReleaseTransient(idx, m_frameIndex);
			return;
		}

		const uint32_t idx = ExternalIndex(image.id);
		if (idx < m_externalImages.size())
		{
			m_externalImages[idx] = {};
		}
	}

	// ── Compilation ──────────────────────────────────────────────────────────

	void RenderGraph::Compile()
	{
		if (!m_compileDirty)
		{
			return;
		}
		m_compileDirty = false;

		const std::size_t N = m_passes.size();
		m_compiled.clear();
		m_compiled.reserve(N);

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
			if (dependent)
				{
					adj[i].push_back(j);
					++inDegree[j];
				}
			}
		}

		std::queue<std::size_t> ready;
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
			const std::size_t cur = ready.front();
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
			AE_WARN(LogCategory::Engine, "RenderGraph: cycle detected - falling back to declaration order.");
			sortedIndices.resize(N);
			std::iota(sortedIndices.begin(), sortedIndices.end(), 0);
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
		        .isCrossFrame = false,
		};
		states[kSwapchainDepthId] = {
		        .layout = gpu::ImageLayout::DepthAttachment,
		        .writeStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT),
		        .writeAccess = static_cast<std::uint64_t>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
		        .isCrossFrame = false,
		};

		for (const std::size_t idx: sortedIndices)
		{
			const PassRecord& pass = m_passes[idx];
			CompiledPass cp;
			cp.passIndex = idx;

			for (const AttachmentRef& a: pass.colorWrites)
			{
				const uint32_t resId = a.image.id;
				constexpr gpu::ImageLayout kTarget = gpu::ImageLayout::ColorAttachment;
				constexpr std::uint64_t kDstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
				constexpr std::uint64_t kDstWrite = static_cast<std::uint64_t>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
				constexpr std::uint64_t kDstReadWrite = static_cast<std::uint64_t>(VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

				const auto it = states.find(resId);
				if (it != states.end())
				{
					const ResourceState& s = it->second;
					const bool layoutChange = (s.layout != kTarget);
					const bool loadRead = (a.loadOp == gpu::LoadOp::Load);
					if (layoutChange || loadRead)
					{
						cp.preBarriers.push_back({
						        .resourceId = resId,
						        .oldLayout = s.layout,
						        .newLayout = kTarget,
						        .srcStage = s.isCrossFrame ? static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) : s.writeStage,
						        .srcAccess = s.isCrossFrame ? static_cast<std::uint64_t>(VK_ACCESS_2_MEMORY_WRITE_BIT) : s.writeAccess,
						        .dstStage = kDstStage,
						        .dstAccess = loadRead ? kDstReadWrite : kDstWrite,
						        .aspect = gpu::ImageAspect::Color,
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
				        .isCrossFrame = false,
				};
			}

			if (pass.depthWrite.has_value())
			{
				const AttachmentRef& da = *pass.depthWrite;
				const uint32_t resId = da.image.id;
				constexpr gpu::ImageLayout kTarget = gpu::ImageLayout::DepthAttachment;
				constexpr std::uint64_t kDepthStages = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
				constexpr std::uint64_t kDepthWrite = static_cast<std::uint64_t>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
				constexpr std::uint64_t kDepthReadWrite = static_cast<std::uint64_t>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

				const auto it = states.find(resId);
				if (it != states.end())
				{
					const ResourceState& s = it->second;
					const bool layoutChange = (s.layout != kTarget);
					const bool loadRead = (da.loadOp == gpu::LoadOp::Load);
					if (layoutChange || loadRead)
					{
						cp.preBarriers.push_back({
						        .resourceId = resId,
						        .oldLayout = s.layout,
						        .newLayout = kTarget,
						        .srcStage = s.isCrossFrame ? static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) : s.writeStage,
						        .srcAccess = s.isCrossFrame ? static_cast<std::uint64_t>(VK_ACCESS_2_MEMORY_WRITE_BIT) : s.writeAccess,
						        .dstStage = kDepthStages,
						        .dstAccess = loadRead ? kDepthReadWrite : kDepthWrite,
						        .aspect = gpu::ImageAspect::Depth,
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
				        .isCrossFrame = false,
				};
			}

			for (const ImageAccessRef& r: pass.imageAccesses)
			{
				const uint32_t resId = r.image.id;

				gpu::ImageLayout targetLayout = gpu::ImageLayout::ShaderReadOnly;
				std::uint64_t dstStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
				std::uint64_t dstAccess = static_cast<std::uint64_t>(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

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

				const auto it = states.find(resId);
				std::uint64_t srcStage;
				std::uint64_t srcAccess;
				gpu::ImageLayout oldLayout;

				if (it != states.end())
				{
					const ResourceState& s = it->second;
					oldLayout = s.layout;
					if (s.isCrossFrame)
					{
						srcStage = static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
						srcAccess = static_cast<std::uint64_t>(VK_ACCESS_2_MEMORY_WRITE_BIT);
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
					aspect = static_cast<gpu::ImageAspect>(m_storage->ResolveTransientAspect(TransientIndex(resId)));
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
				});

				states[resId] = {
				        .layout = targetLayout,
				        .writeStage = dstStage,
				        .writeAccess = dstAccess,
				        .isCrossFrame = false,
				};
			}

			m_compiled.push_back(std::move(cp));
		}

		std::swap(m_lastImageStates, states);
	}

	// ── Execution ────────────────────────────────────────────────────────────

	void RenderGraph::Execute(gpu::CommandList& cmdList, const FrameTarget& target, std::uint64_t frameConstantsAddr, std::uint32_t frameIndex)
	{
		if (m_passes.empty())
		{
			return;
		}

		Compile();

		// Ensure transient images are allocated before building barriers.
		m_storage->EnsureTransientImages(target);

		gpu::CommandList& recorder = cmdList;
		VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(cmdList.GetCommandBuffer());
		gpu::DeviceAddress frameAddr = static_cast<gpu::DeviceAddress>(frameConstantsAddr);

		for (const CompiledPass& cp: m_compiled)
		{
			PassRecord& pass = m_passes[cp.passIndex];
			AE_PROFILE_ZONE_N("RenderPass");
			AE_PROFILE_SET_ZONE_NAME(pass.name.c_str());
			recorder.BeginDebugLabel(pass.name.c_str(), 0.20f, 0.70f, 0.35f, 1.0f);

			// ── Barriers ────────────────────────────────────────────────────
			auto& scratchBarriers = m_storage->GetScratchBarriers();
			scratchBarriers.clear();
			for (const CompiledBarrier& b: cp.preBarriers)
			{
				VkImage image = VK_NULL_HANDLE;
				if (b.resourceId == kSwapchainColorId)
				{
					image = static_cast<VkImage>(target.colorImage);
				}
				else if (b.resourceId == kSwapchainDepthId)
				{
					image = static_cast<VkImage>(target.depthImage);
				}
				else if (IsTransientId(b.resourceId))
				{
					image = m_storage->ResolveTransientImage(TransientIndex(b.resourceId));
				}
				else
				{
					const uint32_t extIdx = ExternalIndex(b.resourceId);
					image = m_storage->GetExternalImage(extIdx);
				}

				if (image == VK_NULL_HANDLE)
				{
					AE_WARN(LogCategory::Vulkan, "RenderGraph: could not resolve image id={} for barrier in pass '{}'.", b.resourceId, pass.name);
					continue;
				}

				scratchBarriers.push_back({
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				        .srcStageMask = static_cast<VkPipelineStageFlags2>(b.srcStage),
				        .srcAccessMask = static_cast<VkAccessFlags2>(b.srcAccess),
				        .dstStageMask = static_cast<VkPipelineStageFlags2>(b.dstStage),
				        .dstAccessMask = static_cast<VkAccessFlags2>(b.dstAccess),
				        .oldLayout = gpu::ToVk(b.oldLayout),
				        .newLayout = gpu::ToVk(b.newLayout),
				        .image = image,
				        .subresourceRange = {gpu::ToVk(b.aspect), 0, 1, 0, 1},
				});
			}
			vkutil::TransitionImages(vkCmd, scratchBarriers.data(), static_cast<uint32_t>(scratchBarriers.size()));

			// ── Dynamic rendering ───────────────────────────────────────────
			auto& scratchColorInfos = m_storage->GetScratchColorInfos();
			scratchColorInfos.clear();
			for (const AttachmentRef& a: pass.colorWrites)
			{
				VkImageView view = VK_NULL_HANDLE;
				if (a.image.id == kSwapchainColorId)
				{
					view = static_cast<VkImageView>(target.colorView);
				}
				else if (IsTransientId(a.image.id))
				{
					view = m_storage->ResolveTransientView(TransientIndex(a.image.id));
				}
				else
				{
					view = m_storage->GetExternalView(ExternalIndex(a.image.id));
				}

				scratchColorInfos.push_back({
				        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				        .imageView = view,
				        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .loadOp = gpu::ToVk(a.loadOp),
				        .storeOp = gpu::ToVk(a.storeOp),
				        .clearValue = gpu::ToVk(a.clearValue),
				});
			}

			VkRenderingAttachmentInfo depthInfo{};
			bool hasDepth = false;
			if (pass.depthWrite.has_value())
			{
				hasDepth = true;
				const AttachmentRef& da = *pass.depthWrite;
				VkImageView depthView = VK_NULL_HANDLE;
				if (da.image.id == kSwapchainDepthId)
				{
					depthView = static_cast<VkImageView>(target.depthView);
				}
				else if (IsTransientId(da.image.id))
				{
					depthView = m_storage->ResolveTransientView(TransientIndex(da.image.id));
				}
				else
				{
					depthView = m_storage->GetExternalView(ExternalIndex(da.image.id));
				}

				depthInfo = {
				        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				        .imageView = depthView,
				        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
				        .loadOp = gpu::ToVk(da.loadOp),
				        .storeOp = gpu::ToVk(da.storeOp),
				        .clearValue = gpu::ToVk(da.clearValue),
				};
			}

			const gpu::Extent2D passExtent = pass.extentOverride.value_or(target.extent);
			const bool useDynamicRendering = pass.kind == PassKind::Graphics && (!scratchColorInfos.empty() || hasDepth);
			if (useDynamicRendering)
			{
				const VkRenderingInfo renderInfo{
				        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
				        .renderArea = {{0, 0}, {passExtent.width, passExtent.height}},
				        .layerCount = 1,
				        .colorAttachmentCount = static_cast<uint32_t>(scratchColorInfos.size()),
				        .pColorAttachments = scratchColorInfos.data(),
				        .pDepthAttachment = hasDepth ? &depthInfo : nullptr,
				};
				cmdList.BeginRendering(&renderInfo);

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
				cmdList.SetViewport(viewport);
				cmdList.SetScissor(scissor);
			}

			if (pass.execute)
			{
				AE_PROFILE_GPU_ZONE_T(m_tracyVkCtx, vkCmd, gpuPassZone, pass.name.c_str());
				const auto t0 = std::chrono::high_resolution_clock::now();
				PassContext ctx{recorder, passExtent, frameAddr, frameIndex};
				pass.execute(ctx);
				const auto t1 = std::chrono::high_resolution_clock::now();
				pass.lastCpuTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
			}

			if (useDynamicRendering)
			{
				cmdList.EndRendering();
			}

			recorder.EndDebugLabel();
		}

		AE_PROFILE_GPU_COLLECT(m_tracyVkCtx, vkCmd);
	}
} // namespace aether
