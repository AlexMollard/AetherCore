#include "RenderGraph.hpp"

#include <algorithm>
#include <numeric>
#include <queue>
#include <unordered_map>

#include "Logger.hpp"
#include "VulkanUtils.hpp"

namespace aether
{
	// ──────────────────────────────────────────────────────────────────────────
	//  PassBuilder
	// ──────────────────────────────────────────────────────────────────────────
	RenderGraph::PassBuilder::PassBuilder(RenderGraph& graph, std::size_t passIndex)
	      : m_graph(graph), m_passIndex(passIndex)
	{
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteColor(RGImage image, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue)
	{
		m_graph.m_passes[m_passIndex].colorWrites.push_back(AttachmentRef{
		        .image = image,
		        .loadOp = loadOp,
		        .storeOp = storeOp,
		        .clearValue = clearValue,
		});
		m_graph.m_dirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteDepth(RGImage image, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue)
	{
		m_graph.m_passes[m_passIndex].depthWrite = AttachmentRef{
			.image = image,
			.loadOp = loadOp,
			.storeOp = storeOp,
			.clearValue = clearValue,
		};
		m_graph.m_dirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadTexture(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::SampledRead,
		});
		m_graph.m_dirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadTextureCompute(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::SampledRead,
		});
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
		m_graph.m_dirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadStorageImage(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageRead,
		});
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
		m_graph.m_dirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteStorageImage(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageWrite,
		});
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
		m_graph.m_dirty = true;
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

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::SetExtent(VkExtent2D extent)
	{
		m_graph.m_passes[m_passIndex].extentOverride = extent;
		m_graph.m_dirty = true;
		return *this;
	}

	// ──────────────────────────────────────────────────────────────────────────
	//  RenderGraph — pass management
	// ──────────────────────────────────────────────────────────────────────────
	RenderGraph::PassBuilder RenderGraph::AddPass(std::string name)
	{
		m_passes.push_back(PassRecord{ .name = std::move(name) });
		m_dirty = true;
		return PassBuilder{ *this, m_passes.size() - 1 };
	}

	RenderGraph::PassBuilder RenderGraph::AddComputePass(std::string name)
	{
		m_passes.push_back(PassRecord{
		        .name = std::move(name),
		        .kind = PassKind::Compute,
		});
		m_dirty = true;
		return PassBuilder{ *this, m_passes.size() - 1 };
	}

	RGImage RenderGraph::RegisterImage(VkImage image, VkImageView view, VkImageAspectFlags aspect)
	{
		const uint32_t id = kFirstExternalId + static_cast<uint32_t>(m_externalImages.size());
		m_externalImages.push_back({ image, view, aspect });
		m_dirty = true;
		return RGImage{ id };
	}

	void RenderGraph::RemovePass(const std::string& name)
	{
		const auto it = std::find_if(m_passes.begin(), m_passes.end(), [&](const PassRecord& p) { return p.name == name; });
		if (it != m_passes.end())
		{
			m_passes.erase(it);
			m_dirty = true;
		}
	}

	bool RenderGraph::HasPass(std::string_view name) const
	{
		return std::find_if(m_passes.begin(), m_passes.end(), [&](const PassRecord& p) { return p.name == name; }) != m_passes.end();
	}

	void RenderGraph::Clear()
	{
		m_passes.clear();
		m_compiled.clear();
		m_dirty = true;
	}

	// ──────────────────────────────────────────────────────────────────────────
	//  RenderGraph — frame execution
	// ──────────────────────────────────────────────────────────────────────────
	void RenderGraph::Execute(VkCommandBuffer cmd, const FrameTarget& target, VkDeviceAddress frameConstantsAddr, std::uint32_t frameIndex)
	{
		if (m_passes.empty())
		{
			return;
		}

		if (m_dirty)
		{
			Compile();
		}

		CommandRecorder recorder{ cmd };

		for (const CompiledPass& cp: m_compiled)
		{
			const PassRecord& pass = m_passes[cp.passIndex];
			recorder.BeginDebugLabel(pass.name.c_str(), 0.20f, 0.70f, 0.35f, 1.0f);

			// ── Pre-pass image barriers ──────────────────────────────────────
			for (const CompiledBarrier& b: cp.preBarriers)
			{
				const VkImage image = ResolveImage(b.resourceId, target);
				if (image == VK_NULL_HANDLE)
				{
					WARN(LogCategory::Vulkan,
					        "RenderGraph: could not resolve image id={} for barrier in pass "
					        "'{}'.",
					        b.resourceId,
					        pass.name);
					continue;
				}
				vkutil::TransitionImage(cmd, image, b.oldLayout, b.newLayout, b.srcStage, b.srcAccess, b.dstStage, b.dstAccess, b.aspect);
			}

			// ── Build VkRenderingAttachmentInfo arrays ───────────────────────
			std::vector<VkRenderingAttachmentInfo> colorInfos;
			colorInfos.reserve(pass.colorWrites.size());
			for (const AttachmentRef& a: pass.colorWrites)
			{
				colorInfos.push_back({
				        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				        .imageView = ResolveView(a.image.id, target),
				        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .loadOp = a.loadOp,
				        .storeOp = a.storeOp,
				        .clearValue = a.clearValue,
				});
			}

			VkRenderingAttachmentInfo depthInfo{};
			bool hasDepth = false;
			if (pass.depthWrite.has_value())
			{
				hasDepth = true;
				const AttachmentRef& da = *pass.depthWrite;
				depthInfo = {
					.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
					.imageView = ResolveView(da.image.id, target),
					.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
					.loadOp = da.loadOp,
					.storeOp = da.storeOp,
					.clearValue = da.clearValue,
				};
			}

			// Resolve the effective render extent for this pass.
			const VkExtent2D passExtent = pass.extentOverride.value_or(target.extent);

			const bool useDynamicRendering = pass.kind == PassKind::Graphics && (!colorInfos.empty() || hasDepth);
			if (useDynamicRendering)
			{
				// ── Begin dynamic rendering ──────────────────────────────────────
				const VkRenderingInfo renderInfo{
					.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
					.renderArea = { { 0, 0 }, passExtent },
					.layerCount = 1,
					.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()),
					.pColorAttachments = colorInfos.data(),
					.pDepthAttachment = hasDepth ? &depthInfo : nullptr,
				};
				vkCmdBeginRendering(cmd, &renderInfo);

				const VkViewport viewport{
					.x = 0.0f,
					.y = 0.0f,
					.width = static_cast<float>(passExtent.width),
					.height = static_cast<float>(passExtent.height),
					.minDepth = 0.0f,
					.maxDepth = 1.0f,
				};
				const VkRect2D scissor{
					{ 0, 0 },
                    passExtent
				};
				vkCmdSetViewport(cmd, 0, 1, &viewport);
				vkCmdSetScissor(cmd, 0, 1, &scissor);
			}

			// ── Execute callback ─────────────────────────────────────────────
			if (pass.execute)
			{
				PassContext ctx{ recorder, passExtent, frameConstantsAddr, frameIndex };
				pass.execute(ctx);
			}

			if (useDynamicRendering)
			{
				vkCmdEndRendering(cmd);
			}

			recorder.EndDebugLabel();
		}
	}

	// ──────────────────────────────────────────────────────────────────────────
	//  Compile: topological sort + inter-pass barrier derivation
	// ──────────────────────────────────────────────────────────────────────────
	void RenderGraph::Compile()
	{
		const std::size_t N = m_passes.size();
		m_compiled.clear();
		m_compiled.reserve(N);

		// ── Kahn's topological sort ──────────────────────────────────────────
		// Edge i→j: pass i writes to a resource that pass j also accesses.
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
			if (m_passes[idx].depthWrite.has_value())
			{
				if (m_passes[idx].depthWrite->image.id == resId)
				{
					return true;
				}
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
				if (!dependent && m_passes[i].depthWrite.has_value())
				{
					if (passAccesses(j, m_passes[i].depthWrite->image.id))
					{
						dependent = true;
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
			WARN(LogCategory::Engine, "RenderGraph: cycle detected — falling back to declaration order.");
			sortedIndices.resize(N);
			std::iota(sortedIndices.begin(), sortedIndices.end(), 0);
		}

		// ── Per-resource state tracking for barrier derivation ───────────────
		// Seed with the layouts established by Swapchain::BeginFrame transitions.
		struct ResourceState
		{
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkPipelineStageFlags2 writeStage = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 writeAccess = VK_ACCESS_2_NONE;
		};

		std::unordered_map<uint32_t, ResourceState> states;
		states[kSwapchainColorId] = {
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		};
		states[kSwapchainDepthId] = {
			VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		};

		for (const std::size_t idx: sortedIndices)
		{
			const PassRecord& pass = m_passes[idx];
			CompiledPass cp;
			cp.passIndex = idx;

			// --- Color attachment writes ------------------------------------
			for (const AttachmentRef& a: pass.colorWrites)
			{
				const uint32_t resId = a.image.id;
				constexpr VkImageLayout kTarget = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

				const auto it = states.find(resId);
				if (it != states.end())
				{
					const ResourceState& s = it->second;
					const bool layoutChange = (s.layout != kTarget);
					const bool loadRead = (a.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
					if (layoutChange || loadRead)
					{
						cp.preBarriers.push_back({
						        .resourceId = resId,
						        .oldLayout = s.layout,
						        .newLayout = kTarget,
						        .srcStage = s.writeStage,
						        .srcAccess = s.writeAccess,
						        .dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
						        .dstAccess = loadRead ? (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
						        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
						});
					}
				}
				else
				{
					cp.preBarriers.push_back({
					        .resourceId = resId,
					        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					        .newLayout = kTarget,
					        .srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
					        .srcAccess = VK_ACCESS_2_NONE,
					        .dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					        .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
					        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
					});
				}

				states[resId] = {
					kTarget,
					VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				};
			}

			// --- Depth attachment write ------------------------------------
			if (pass.depthWrite.has_value())
			{
				const AttachmentRef& da = *pass.depthWrite;
				const uint32_t resId = da.image.id;
				constexpr VkImageLayout kTarget = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
				constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

				const auto it = states.find(resId);
				if (it != states.end())
				{
					const ResourceState& s = it->second;
					const bool layoutChange = (s.layout != kTarget);
					const bool loadRead = (da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
					if (layoutChange || loadRead)
					{
						cp.preBarriers.push_back({
						        .resourceId = resId,
						        .oldLayout = s.layout,
						        .newLayout = kTarget,
						        .srcStage = s.writeStage,
						        .srcAccess = s.writeAccess,
						        .dstStage = kDepthStages,
						        .dstAccess = loadRead ? (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
						        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
						});
					}
				}
				else
				{
					cp.preBarriers.push_back({
					        .resourceId = resId,
					        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					        .newLayout = kTarget,
					        .srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
					        .srcAccess = VK_ACCESS_2_NONE,
					        .dstStage = kDepthStages,
					        .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
					});
				}

				states[resId] = {
					kTarget,
					kDepthStages,
					VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				};
			}

			// --- General image accesses (sampled + storage) -----------------
			for (const ImageAccessRef& r: pass.imageAccesses)
			{
				const uint32_t resId = r.image.id;

				VkImageLayout targetLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
				VkAccessFlags2 dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

				switch (r.type)
				{
					case ImageAccessType::SampledRead:
						dstStage = (pass.kind == PassKind::Compute) ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
						dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
						targetLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
						break;
					case ImageAccessType::StorageRead:
						dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
						dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
						targetLayout = VK_IMAGE_LAYOUT_GENERAL;
						break;
					case ImageAccessType::StorageWrite:
						dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
						dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
						targetLayout = VK_IMAGE_LAYOUT_GENERAL;
						break;
				}

				const auto it = states.find(resId);
				const VkPipelineStageFlags2 srcStage = (it != states.end()) ? it->second.writeStage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
				const VkAccessFlags2 srcAccess = (it != states.end()) ? it->second.writeAccess : VK_ACCESS_2_NONE;
				const VkImageLayout oldLayout = (it != states.end()) ? it->second.layout : VK_IMAGE_LAYOUT_UNDEFINED;

				cp.preBarriers.push_back({
				        .resourceId = resId,
				        .oldLayout = oldLayout,
				        .newLayout = targetLayout,
				        .srcStage = srcStage,
				        .srcAccess = srcAccess,
				        .dstStage = dstStage,
				        .dstAccess = dstAccess,
				        .aspect = ResolveAspect(resId),
				});

				if (r.type == ImageAccessType::StorageWrite)
				{
					states[resId] = {
						targetLayout,
						dstStage,
						dstAccess,
					};
				}
			}

			m_compiled.push_back(std::move(cp));
		}

		INFO(LogCategory::Engine, "RenderGraph compiled: {} pass(es).", m_compiled.size());
		m_dirty = false;
	}

	// ──────────────────────────────────────────────────────────────────────────
	//  Resolution helpers
	// ──────────────────────────────────────────────────────────────────────────
	VkImage RenderGraph::ResolveImage(uint32_t resourceId, const FrameTarget& target) const
	{
		if (resourceId == kSwapchainColorId)
		{
			return target.colorImage;
		}
		if (resourceId == kSwapchainDepthId)
		{
			return target.depthImage;
		}
		const uint32_t idx = resourceId - kFirstExternalId;
		if (idx < m_externalImages.size())
		{
			return m_externalImages[idx].image;
		}
		return VK_NULL_HANDLE;
	}

	VkImageView RenderGraph::ResolveView(uint32_t resourceId, const FrameTarget& target) const
	{
		if (resourceId == kSwapchainColorId)
		{
			return target.colorView;
		}
		if (resourceId == kSwapchainDepthId)
		{
			return target.depthView;
		}
		const uint32_t idx = resourceId - kFirstExternalId;
		if (idx < m_externalImages.size())
		{
			return m_externalImages[idx].view;
		}
		return VK_NULL_HANDLE;
	}

	VkImageAspectFlags RenderGraph::ResolveAspect(uint32_t resourceId) const
	{
		if (resourceId == kSwapchainDepthId)
		{
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		const uint32_t idx = resourceId - kFirstExternalId;
		if (idx < m_externalImages.size())
		{
			return m_externalImages[idx].aspect;
		}
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
} // namespace aether
