#include "rendering/RenderGraph.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>

#include "gpu/BindlessManager.hpp"
#include "rendering/CommandRecorder.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether
{
	void RenderGraph::Initialize(VkDevice device, VmaAllocator allocator)
	{
		m_device = device;
		m_allocator = allocator;
	}

	void RenderGraph::Shutdown()
	{
		for (TransientImageEntry& entry: m_transientImages)
		{
			entry.image.Reset();
			entry.aliasPhysicalIndex = 0xFFFFFFFFu;
			entry.allocatedExtent = {};
		}
		for (TransientPhysicalImage& physical: m_transientPhysicalImages)
		{
			physical.image.Reset();
			physical.allocatedExtent = {};
		}
		m_transientPhysicalImages.clear();
		m_transientImages.clear();
		m_externalImages.clear();
		m_passes.clear();
		m_compiled.clear();
		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
	}

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
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadStorageImage(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageRead,
		});
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteStorageImage(RGImage image)
	{
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageWrite,
		});
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
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
		return *this;
	}

	RenderGraph::PassBuilder RenderGraph::AddPass(std::string name)
	{
		m_passes.push_back(PassRecord{ .name = std::move(name) });
		return PassBuilder{ *this, m_passes.size() - 1 };
	}

	RenderGraph::PassBuilder RenderGraph::AddComputePass(std::string name)
	{
		m_passes.push_back(PassRecord{
		        .name = std::move(name),
		        .kind = PassKind::Compute,
		});
		return PassBuilder{ *this, m_passes.size() - 1 };
	}

	RGImage RenderGraph::RegisterImage(VkImage image, VkImageView view, VkImageAspectFlags aspect)
	{
		const uint32_t id = kFirstExternalId + static_cast<uint32_t>(m_externalImages.size());
		m_externalImages.push_back({ image, view, aspect });
		return RGImage{ id };
	}

	RGImage RenderGraph::CreateTransientImage(const TransientImageDesc& desc)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraph: CreateTransientImage called before Initialize().");
		}

		TransientImageEntry entry{};
		entry.desc = desc;
		m_transientImages.push_back(std::move(entry));
		const uint32_t id = kFirstTransientId + static_cast<uint32_t>(m_transientImages.size() - 1);
		return RGImage{ id };
	}

	RGImage RenderGraph::CreateTransientColor(VkFormat format, VkExtent2D extent, VkImageUsageFlags extraUsage)
	{
		return CreateTransientImage({
		        .format = format,
		        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | extraUsage,
		        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
		        .extent = extent,
		});
	}

	RGImage RenderGraph::CreateTransientDepth(VkFormat format, VkExtent2D extent, VkImageUsageFlags extraUsage)
	{
		return CreateTransientImage({
		        .format = format,
		        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | extraUsage,
		        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
		        .extent = extent,
		});
	}

	std::uint32_t RenderGraph::EnsureBindlessSampled(RGImage image, BindlessManager& bindlessManager, VkDevice device, VkImageLayout descriptorLayout)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			return 0xFFFFFFFFu;
		}
		if (!IsTransientId(image.id))
		{
			return 0xFFFFFFFFu;
		}

		const uint32_t idx = image.id - kFirstTransientId;
		if (idx >= m_transientImages.size())
		{
			return 0xFFFFFFFFu;
		}

		TransientImageEntry& entry = m_transientImages[idx];
		entry.bindlessRequested = true;
		entry.bindlessLayout = descriptorLayout;
		entry.aliasPhysicalIndex = 0xFFFFFFFFu;

		if (!entry.image)
		{
			if (entry.desc.format == VK_FORMAT_UNDEFINED || entry.desc.usage == 0 || entry.desc.extent.width == 0 || entry.desc.extent.height == 0)
			{
				return 0xFFFFFFFFu;
			}

			AE_EXPECT_OR_THROW(newImage,
			        UniqueImage::Create(m_device,
			                m_allocator,
			                {
			                        .extent = entry.desc.extent,
			                        .format = entry.desc.format,
			                        .usage = entry.desc.usage,
			                }));
			entry.image = std::move(newImage);
			entry.allocatedExtent = entry.desc.extent;
		}

		AE_EXPECT_OR_THROW_VOID(entry.image.EnsureBindlessSampled(bindlessManager, device, entry.desc.aspect, descriptorLayout));
		return entry.image.GetBindlessSampledSlot();
	}

	std::uint32_t RenderGraph::GetBindlessSampledSlot(RGImage image) const
	{
		if (!IsTransientId(image.id))
		{
			return 0xFFFFFFFFu;
		}

		const uint32_t idx = image.id - kFirstTransientId;
		if (idx >= m_transientImages.size())
		{
			return 0xFFFFFFFFu;
		}

		const TransientImageEntry& entry = m_transientImages[idx];
		if (!entry.image.HasBindlessSampled())
		{
			return 0xFFFFFFFFu;
		}
		return entry.image.GetBindlessSampledSlot();
	}

	void RenderGraph::ReleaseImage(const RGImage image)
	{
		if (image.id == kSwapchainColorId || image.id == kSwapchainDepthId || image.id == RGImage::kInvalid)
		{
			return;
		}

		if (IsTransientId(image.id))
		{
			const uint32_t idx = image.id - kFirstTransientId;
			if (idx < m_transientImages.size())
			{
				m_transientImages[idx].image.Reset();
				m_transientImages[idx].bindlessRequested = false;
				m_transientImages[idx].bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				m_transientImages[idx].aliasPhysicalIndex = 0xFFFFFFFFu;
				m_transientImages[idx].allocatedExtent = {};
				m_transientImages[idx].desc = {};
					}
			return;
		}

		const uint32_t idx = image.id - kFirstExternalId;
		if (idx < m_externalImages.size())
		{
			m_externalImages[idx] = {};
			}
	}

	void RenderGraph::RemovePass(const std::string& name)
	{
		const auto it = std::find_if(m_passes.begin(), m_passes.end(), [&](const PassRecord& p) { return p.name == name; });
		if (it != m_passes.end())
		{
			m_passes.erase(it);
			}
	}

	bool RenderGraph::HasPass(std::string_view name) const
	{
		return std::find_if(m_passes.begin(), m_passes.end(), [&](const PassRecord& p) { return p.name == name; }) != m_passes.end();
	}

	void RenderGraph::Clear()
	{
		for (TransientImageEntry& entry: m_transientImages)
		{
			entry.image.Reset();
			entry.aliasPhysicalIndex = 0xFFFFFFFFu;
			entry.allocatedExtent = {};
		}
		for (TransientPhysicalImage& physical: m_transientPhysicalImages)
		{
			physical.image.Reset();
			physical.allocatedExtent = {};
		}
		m_transientPhysicalImages.clear();
		m_transientImages.clear();
		m_externalImages.clear();
		m_passes.clear();
		m_compiled.clear();
	}

	void RenderGraph::EnsureTransientImages(const FrameTarget& target)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraph: transient images require Initialize(device, allocator).");
			return;
		}

		struct Lifetime
		{
			int first = std::numeric_limits<int>::max();
			int last = -1;
		};

		std::vector<Lifetime> lifetimes(m_transientImages.size());
		auto touch = [&](const uint32_t resourceId, const int passOrder)
		{
			if (!IsTransientId(resourceId))
			{
				return;
			}
			const uint32_t idx = resourceId - kFirstTransientId;
			if (idx >= m_transientImages.size())
			{
				return;
			}
			lifetimes[idx].first = std::min(lifetimes[idx].first, passOrder);
			lifetimes[idx].last = std::max(lifetimes[idx].last, passOrder);
		};

		for (std::size_t order = 0; order < m_compiled.size(); ++order)
		{
			const PassRecord& pass = m_passes[m_compiled[order].passIndex];
			for (const AttachmentRef& a: pass.colorWrites)
			{
				touch(a.image.id, static_cast<int>(order));
			}
			if (pass.depthWrite.has_value())
			{
				touch(pass.depthWrite->image.id, static_cast<int>(order));
			}
			for (const ImageAccessRef& access: pass.imageAccesses)
			{
				touch(access.image.id, static_cast<int>(order));
			}
		}

		for (std::uint32_t idx = 0; idx < m_transientImages.size(); ++idx)
		{
			TransientImageEntry& entry = m_transientImages[idx];
			entry.aliasPhysicalIndex = 0xFFFFFFFFu;
			if (!entry.bindlessRequested)
			{
				entry.image.Reset();
				entry.allocatedExtent = {};
			}
		}

		std::vector<std::uint32_t> candidates;
		candidates.reserve(m_transientImages.size());
		for (std::uint32_t idx = 0; idx < m_transientImages.size(); ++idx)
		{
			if (m_transientImages[idx].bindlessRequested)
			{
				continue;
			}
			if (lifetimes[idx].last >= lifetimes[idx].first)
			{
				candidates.push_back(idx);
			}
		}
		std::sort(candidates.begin(), candidates.end(), [&](const std::uint32_t a, const std::uint32_t b) { return lifetimes[a].first < lifetimes[b].first; });

		std::vector<int> physicalLastUse(m_transientPhysicalImages.size(), -1);

		for (const std::uint32_t idx: candidates)
		{
			TransientImageEntry& entry = m_transientImages[idx];
			const VkExtent2D requestedExtent = (entry.desc.extent.width == 0 || entry.desc.extent.height == 0) ? target.extent : entry.desc.extent;

			if (entry.desc.format == VK_FORMAT_UNDEFINED || entry.desc.usage == 0 || requestedExtent.width == 0 || requestedExtent.height == 0)
			{
				AE_WARN(LogCategory::Engine, "RenderGraph: skipping invalid transient image desc (format={}, usage=0x{:X}, extent={}x{}).", static_cast<int>(entry.desc.format), entry.desc.usage, requestedExtent.width, requestedExtent.height);
				continue;
			}

			const int firstUse = lifetimes[idx].first;
			const int lastUse = lifetimes[idx].last;
			std::uint32_t chosen = 0xFFFFFFFFu;

			for (std::uint32_t p = 0; p < m_transientPhysicalImages.size(); ++p)
			{
				const TransientPhysicalImage& phys = m_transientPhysicalImages[p];
				if (phys.desc.format != entry.desc.format || phys.desc.usage != entry.desc.usage || phys.desc.aspect != entry.desc.aspect)
				{
					continue;
				}
				if (phys.allocatedExtent.width != requestedExtent.width || phys.allocatedExtent.height != requestedExtent.height)
				{
					continue;
				}
				if (physicalLastUse[p] >= firstUse)
				{
					continue;
				}
				chosen = p;
				break;
			}

			if (chosen == 0xFFFFFFFFu)
			{
				chosen = static_cast<std::uint32_t>(m_transientPhysicalImages.size());
				m_transientPhysicalImages.push_back(TransientPhysicalImage{
				        .desc = entry.desc,
				        .allocatedExtent = requestedExtent,
				});
				physicalLastUse.push_back(-1);
			}

			TransientPhysicalImage& phys = m_transientPhysicalImages[chosen];
			const bool needsCreate = !phys.image || phys.allocatedExtent.width != requestedExtent.width || phys.allocatedExtent.height != requestedExtent.height;
			if (needsCreate)
			{
				phys.image.Reset();
				AE_EXPECT_OR_THROW(newImage,
				        UniqueImage::Create(m_device,
				                m_allocator,
				                {
				                        .extent = requestedExtent,
				                        .format = entry.desc.format,
				                        .usage = entry.desc.usage,
				                }));
				phys.image = std::move(newImage);
				phys.allocatedExtent = requestedExtent;
				phys.desc = entry.desc;
				const std::string physName = std::format("RenderGraph.Transient.Physical[{}]", chosen);
				phys.image.SetName(m_device, physName.c_str());
			}

			entry.aliasPhysicalIndex = chosen;
			physicalLastUse[chosen] = lastUse;
		}

		for (std::uint32_t entryIdx = 0; entryIdx < m_transientImages.size(); ++entryIdx)
		{
			TransientImageEntry& entry = m_transientImages[entryIdx];
			if (!entry.bindlessRequested)
			{
				continue;
			}

			const VkExtent2D requestedExtent = (entry.desc.extent.width == 0 || entry.desc.extent.height == 0) ? target.extent : entry.desc.extent;
			const bool needsCreate = !entry.image || entry.allocatedExtent.width != requestedExtent.width || entry.allocatedExtent.height != requestedExtent.height;
			if (!needsCreate)
			{
				continue;
			}

			if (entry.desc.format == VK_FORMAT_UNDEFINED || entry.desc.usage == 0 || requestedExtent.width == 0 || requestedExtent.height == 0)
			{
				continue;
			}

			entry.image.Reset();
			AE_EXPECT_OR_THROW(newImage,
			        UniqueImage::Create(m_device,
			                m_allocator,
			                {
			                        .extent = requestedExtent,
			                        .format = entry.desc.format,
			                        .usage = entry.desc.usage,
			                }));
			entry.image = std::move(newImage);
			entry.allocatedExtent = requestedExtent;
			const std::string entryName = std::format("RenderGraph.Transient.Bindless[{}]", entryIdx);
			entry.image.SetName(m_device, entryName.c_str());
		}
	}

	void RenderGraph::Execute(CommandRecorder& recorder, const FrameTarget& target, std::uint64_t frameConstantsAddr, std::uint32_t frameIndex)
	{
		if (m_passes.empty())
		{
			return;
		}

		Compile();

		EnsureTransientImages(target);

		VkDeviceAddress frameAddr = static_cast<VkDeviceAddress>(frameConstantsAddr);

		for (const CompiledPass& cp: m_compiled)
		{
			const PassRecord& pass = m_passes[cp.passIndex];
			AE_PROFILE_ZONE_N("RenderPass");
			AE_PROFILE_SET_ZONE_NAME(pass.name.c_str());
			recorder.BeginDebugLabel(pass.name.c_str(), 0.20f, 0.70f, 0.35f, 1.0f);

			for (const CompiledBarrier& b: cp.preBarriers)
			{
				const VkImage image = ResolveImage(b.resourceId, target);
				if (image == VK_NULL_HANDLE)
				{
					AE_WARN(LogCategory::Vulkan, "RenderGraph: could not resolve image id={} for barrier in pass '{}'.", b.resourceId, pass.name);
					continue;
				}
				vkutil::TransitionImage(recorder.GetCommandBuffer(), image, b.oldLayout, b.newLayout, b.srcStage, b.srcAccess, b.dstStage, b.dstAccess, b.aspect);
			}

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

			const VkExtent2D passExtent = pass.extentOverride.value_or(target.extent);
			const bool useDynamicRendering = pass.kind == PassKind::Graphics && (!colorInfos.empty() || hasDepth);
			if (useDynamicRendering)
			{
				const VkRenderingInfo renderInfo{
					.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
					.renderArea = { { 0, 0 }, passExtent },
					.layerCount = 1,
					.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()),
					.pColorAttachments = colorInfos.data(),
					.pDepthAttachment = hasDepth ? &depthInfo : nullptr,
				};
				vkCmdBeginRendering(recorder.GetCommandBuffer(), &renderInfo);

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
					passExtent,
				};
				vkCmdSetViewport(recorder.GetCommandBuffer(), 0, 1, &viewport);
				vkCmdSetScissor(recorder.GetCommandBuffer(), 0, 1, &scissor);
			}

			if (pass.execute)
			{
				PassContext ctx{ recorder, passExtent, frameAddr, frameIndex };
				pass.execute(ctx);
			}

			if (useDynamicRendering)
			{
				vkCmdEndRendering(recorder.GetCommandBuffer());
			}

			recorder.EndDebugLabel();
		}
	}

	void RenderGraph::Compile()
	{
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

		// Carry over end-of-frame state from the prior frame. Images that
		// persist across frames (external images, transient depth) need their
		// actual GPU layout tracked so the first-access barrier in this frame
		// uses the correct oldLayout.
		std::unordered_map<uint32_t, ResourceState> states;
		for (const auto& [id, s]: m_lastImageStates)
		{
			states[id] = {
				s.layout,
				s.writeStage,
				s.writeAccess,
				true, // isCrossFrame
			};
		}

		// Pre-seed swapchain images with their resting layout. Overrides any
		// loaded state — swapchain images are re-acquired each frame and their
		// layout is managed externally.
		states[kSwapchainColorId] = {
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			false, // isCrossFrame
		};
		states[kSwapchainDepthId] = {
			VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			false, // isCrossFrame
		};

		for (const std::size_t idx: sortedIndices)
		{
			const PassRecord& pass = m_passes[idx];
			CompiledPass cp;
			cp.passIndex = idx;

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
						        .srcStage = s.isCrossFrame ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : s.writeStage,
						        .srcAccess = s.isCrossFrame ? VK_ACCESS_2_MEMORY_WRITE_BIT : s.writeAccess,
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
					false, // isCrossFrame
				};
			}

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
						        .srcStage = s.isCrossFrame ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : s.writeStage,
						        .srcAccess = s.isCrossFrame ? VK_ACCESS_2_MEMORY_WRITE_BIT : s.writeAccess,
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
					false, // isCrossFrame
				};
			}

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
				VkPipelineStageFlags2 srcStage;
				VkAccessFlags2 srcAccess;
				VkImageLayout oldLayout;

				if (it != states.end())
				{
					const ResourceState& s = it->second;
					oldLayout = s.layout;
					if (s.isCrossFrame)
					{
						srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
						srcAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
					}
					else
					{
						srcStage = s.writeStage;
						srcAccess = s.writeAccess;
					}
				}
				else
				{
					oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
					srcAccess = VK_ACCESS_2_NONE;
				}

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

				states[resId] = {
					targetLayout,
					dstStage,
					dstAccess,
					false, // isCrossFrame
				};
			}

			m_compiled.push_back(std::move(cp));
		}

		// AE_INFO(LogCategory::Engine, "RenderGraph compiled: {} pass(es).", m_compiled.size());
		m_lastImageStates = states;
	}

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
		if (IsTransientId(resourceId))
		{
			const uint32_t idx = resourceId - kFirstTransientId;
			if (idx < m_transientImages.size())
			{
				const TransientImageEntry& entry = m_transientImages[idx];
				if (entry.image)
				{
					return entry.image.Get();
				}
				if (entry.aliasPhysicalIndex < m_transientPhysicalImages.size())
				{
					return m_transientPhysicalImages[entry.aliasPhysicalIndex].image.Get();
				}
			}
			return VK_NULL_HANDLE;
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
		if (IsTransientId(resourceId))
		{
			const uint32_t idx = resourceId - kFirstTransientId;
			if (idx < m_transientImages.size())
			{
				const TransientImageEntry& entry = m_transientImages[idx];
				if (entry.image)
				{
					return entry.image.GetDefaultView();
				}
				if (entry.aliasPhysicalIndex < m_transientPhysicalImages.size())
				{
					return m_transientPhysicalImages[entry.aliasPhysicalIndex].image.GetDefaultView();
				}
			}
			return VK_NULL_HANDLE;
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
		if (IsTransientId(resourceId))
		{
			const uint32_t idx = resourceId - kFirstTransientId;
			if (idx < m_transientImages.size())
			{
				return m_transientImages[idx].desc.aspect;
			}
			return VK_IMAGE_ASPECT_COLOR_BIT;
		}
		const uint32_t idx = resourceId - kFirstExternalId;
		if (idx < m_externalImages.size())
		{
			return m_externalImages[idx].aspect;
		}
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}

	bool RenderGraph::IsTransientId(const uint32_t resourceId) const
	{
		return resourceId >= kFirstTransientId;
	}
} // namespace aether
