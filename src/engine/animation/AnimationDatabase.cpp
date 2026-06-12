#include "animation/AnimationDatabase.hpp"

#include <algorithm>

#include "gpu/GpuTypes.hpp"
#include "utils/Logger.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	// Helper: upload a non-empty CPU array into the heap and return its device address.
	// Returns 0 when the input span is empty (no allocation made).
	template<typename T>
	static gpu::DeviceAddress UploadArray(GpuHeap& heap, const std::vector<T>& data, VkDevice device, VkQueue queue, gpu::CommandPool pool)
	{
		if (data.empty())
		{
			return 0;
		}
		GpuSpan<T> span = heap.Alloc<T>(static_cast<std::uint32_t>(data.size()));
		assert(span.IsValid() && "GpuHeap allocation failed - heap capacity insufficient");
		heap.Upload(span, std::span<const T>(data), device, queue, static_cast<VkCommandPool>(pool));
		return span.address;
	}

	AnimationDatabase AnimationDatabase::Create(const VulkanContext& ctx, gpu::CommandPool uploadPool, const assets::GltfAsset& asset)
	{
		AnimationDatabase db;
		db.m_ctx = &ctx;
		db.m_nodeCount = static_cast<std::uint32_t>(asset.nodes.size());
		db.m_skinCount = static_cast<std::uint32_t>(asset.skins.size());

		if (asset.animations.empty())
		{
			return std::move(db);
		}

		// Validate animation channel node indices before building GPU data.
		const std::uint32_t numNodes = static_cast<std::uint32_t>(asset.nodes.size());
		std::uint32_t clampedChannels = 0;
		for (const auto& clip: asset.animations)
		{
			for (const auto& ch: clip.channels)
			{
				if (ch.nodeIndex >= numNodes)
				{
					AE_WARN(LogCategory::Engine, "AnimationDatabase: channel nodeIndex {} out of range (numNodes={}) in clip '{}'. Clamping to {}.", ch.nodeIndex, numNodes, clip.name, numNodes - 1);
					++clampedChannels;
				}
			}
		}
		if (clampedChannels > 0)
		{
			AE_WARN(LogCategory::Engine, "AnimationDatabase: {} channels were clamped out-of-range. {} nodes, {} skins, {} animations.", clampedChannels, numNodes, asset.skins.size(), asset.animations.size());
		}
		else
		{
			AE_VERBOSE(LogCategory::Engine, "AnimationDatabase: all channels valid (numNodes={}, {} clips, {} skins)", numNodes, asset.animations.size(), asset.skins.size());
		}

		// -- Build CPU arrays -------------------------------------------------
		std::vector<GpuClip> gpuClips;
		std::vector<GpuChannel> gpuChannels;
		std::vector<float> allTimes;
		std::vector<glm::vec4> allValues;
		std::vector<std::int32_t> nodeParents;
		std::vector<glm::vec4> bindTranslations;
		std::vector<glm::vec4> bindRotations;
		std::vector<glm::vec4> bindScales;
		std::vector<GpuSkinMeta> skinMetas;
		std::vector<std::uint32_t> skinJoints;
		std::vector<glm::mat4> skinInverseBinds;
		std::string allStrings; // last - may have non-4-multiple byte count

		std::uint32_t currentChannelOffset = 0;

		for (const auto& srcClip: asset.animations)
		{
			GpuClip gpuClip{};
			gpuClip.nameOffset = static_cast<std::uint32_t>(allStrings.size());
			gpuClip.nameLength = static_cast<std::uint32_t>(srcClip.name.size());
			gpuClip.channelOffset = currentChannelOffset;
			gpuClip.channelCount = static_cast<std::uint32_t>(srcClip.channels.size());
			gpuClip.duration = 0.0f;

			allStrings += srcClip.name;

			for (const auto& srcCh: srcClip.channels)
			{
				GpuChannel gpuCh{};
				gpuCh.nodeIndex = std::min(srcCh.nodeIndex, numNodes - 1);
				gpuCh.animPath = static_cast<std::uint8_t>(srcCh.path);
				gpuCh.interpolation = static_cast<std::uint8_t>(srcCh.interpolation);
				gpuCh.timesOffset = static_cast<std::uint32_t>(allTimes.size() * sizeof(float));
				gpuCh.timesCount = static_cast<std::uint32_t>(srcCh.times.size());
				allTimes.insert(allTimes.end(), srcCh.times.begin(), srcCh.times.end());

				if (!srcCh.times.empty())
				{
					gpuClip.duration = std::max(gpuClip.duration, srcCh.times.back());
				}

				gpuCh.valuesOffset = static_cast<std::uint32_t>(allValues.size() * sizeof(glm::vec4));
				gpuCh.valuesCount = static_cast<std::uint32_t>(srcCh.values.size());
				allValues.insert(allValues.end(), srcCh.values.begin(), srcCh.values.end());

				gpuChannels.push_back(gpuCh);
				++currentChannelOffset;
			}

			gpuClips.push_back(gpuClip);
		}

		db.m_clips = gpuClips;
		db.m_clipNames = allStrings;

		nodeParents.reserve(asset.nodes.size());
		bindTranslations.reserve(asset.nodes.size());
		bindRotations.reserve(asset.nodes.size());
		bindScales.reserve(asset.nodes.size());

		const std::int32_t maxValidParent = static_cast<std::int32_t>(asset.nodes.size()) - 1;
		for (const auto& n: asset.nodes)
		{
			std::int32_t parent = n.parentIndex;

			// Clamp invalid parent indices to -1 (root) to prevent GPU OOB reads
			if (parent > maxValidParent || parent < -1)
			{
				AE_WARN(LogCategory::Engine, "AnimationDatabase: node parentIndex {} out of range (numNodes={}). Clamping to -1.", parent, asset.nodes.size());
				parent = -1;
			}

			nodeParents.push_back(parent);
			bindTranslations.emplace_back(n.translation, 0.0f);
			bindRotations.emplace_back(n.rotation.x, n.rotation.y, n.rotation.z, n.rotation.w);
			bindScales.emplace_back(n.scale, 0.0f);
		}

		// -- Store node names for cross-skeleton remapping ------------------
		db.m_nodeNames.reserve(asset.nodes.size());
		for (const auto& n: asset.nodes)
		{
			db.m_nodeNames.push_back(n.name);
		}

		// -- Compute node depths for level-by-level flatten ------------------
		static constexpr std::uint32_t kUnsetDepth = UINT32_MAX;
		std::vector<std::uint32_t> nodeDepth(nodeParents.size(), kUnsetDepth);
		for (std::size_t i = 0; i < nodeParents.size(); ++i)
		{
			if (nodeParents[i] < 0)
			{
				nodeDepth[i] = 0;
			}
		}
		bool changed = true;
		std::uint32_t maxDepth = 0;
		while (changed)
		{
			changed = false;
			for (std::size_t i = 0; i < nodeParents.size(); ++i)
			{
				if (nodeDepth[i] != kUnsetDepth)
				{
					continue;
				}
				const int p = nodeParents[i];
				if (p >= 0 && nodeDepth[static_cast<std::size_t>(p)] != kUnsetDepth)
				{
					nodeDepth[i] = nodeDepth[static_cast<std::size_t>(p)] + 1;
					maxDepth = std::max(maxDepth, nodeDepth[i]);
					changed = true;
				}
			}
		}

		std::vector<std::vector<std::uint32_t>> nodesAtDepth(maxDepth + 1);
		for (std::size_t i = 0; i < nodeDepth.size(); ++i)
		{
			if (nodeDepth[i] != kUnsetDepth)
			{
				nodesAtDepth[nodeDepth[i]].push_back(static_cast<std::uint32_t>(i));
			}
		}

		std::vector<std::uint32_t> depthSortedNodes;
		std::vector<AnimationDatabase::DepthRange> depthRanges;
		depthRanges.reserve(maxDepth + 1);
		for (std::uint32_t d = 0; d <= maxDepth; ++d)
		{
			DepthRange r;
			r.startIndex = static_cast<std::uint32_t>(depthSortedNodes.size());
			r.count = static_cast<std::uint32_t>(nodesAtDepth[d].size());
			depthSortedNodes.insert(depthSortedNodes.end(), nodesAtDepth[d].begin(), nodesAtDepth[d].end());
			depthRanges.push_back(r);
		}

		db.m_depthSortedNodes = std::move(depthSortedNodes);
		db.m_depthRanges = std::move(depthRanges);
		db.m_depthCount = maxDepth + 1;

		skinMetas.reserve(asset.skins.size());
		for (const auto& s: asset.skins)
		{
			GpuSkinMeta meta{};
			meta.jointOffset = static_cast<std::uint32_t>(skinJoints.size());
			meta.jointCount = static_cast<std::uint32_t>(s.joints.size());
			meta.inverseBindOffset = static_cast<std::uint32_t>(skinInverseBinds.size() * sizeof(glm::mat4));

			skinJoints.insert(skinJoints.end(), s.joints.begin(), s.joints.end());

			if (!s.inverseBindMatrices.empty())
			{
				skinInverseBinds.insert(skinInverseBinds.end(), s.inverseBindMatrices.begin(), s.inverseBindMatrices.end());
			}
			else
			{
				for (std::size_t i = 0; i < s.joints.size(); ++i)
				{
					skinInverseBinds.emplace_back(1.0f);
				}
			}

			skinMetas.push_back(meta);
		}

		// -- Size the heap and upload all arrays ------------------------------
		// GpuHeap::AllocBytes enforces a 16-byte minimum alignment, so each term
		// must be rounded up to a 16-byte boundary to guarantee enough capacity.
		const auto align16 = [](VkDeviceSize v) -> VkDeviceSize
		{
			return (v + 15) & ~VkDeviceSize(15);
		};
		VkDeviceSize totalBytes = align16(gpuClips.size() * sizeof(GpuClip));
		totalBytes += align16(gpuChannels.size() * sizeof(GpuChannel));
		totalBytes += align16(allTimes.size() * sizeof(float));
		totalBytes += align16(allValues.size() * sizeof(glm::vec4));
		totalBytes += align16(nodeParents.size() * sizeof(std::int32_t));
		totalBytes += align16(bindTranslations.size() * sizeof(glm::vec4));
		totalBytes += align16(bindRotations.size() * sizeof(glm::vec4));
		totalBytes += align16(bindScales.size() * sizeof(glm::vec4));
		totalBytes += align16(skinMetas.size() * sizeof(GpuSkinMeta));
		totalBytes += align16(skinJoints.size() * sizeof(std::uint32_t));
		totalBytes += align16(skinInverseBinds.size() * sizeof(glm::mat4));
		totalBytes += align16(db.m_depthSortedNodes.size() * sizeof(std::uint32_t));
		totalBytes += align16(db.m_depthRanges.size() * sizeof(DepthRange));
		totalBytes += align16(allStrings.size());

		db.m_heap.Initialize(ctx, {.capacityBytes = totalBytes, .debugName = "AnimationDatabase"});

		VkDevice device = ctx.GetDevice().device;
		VkQueue queue = ctx.GetGraphicsQueue();

		db.m_clipsAddr = UploadArray(db.m_heap, gpuClips, device, queue, uploadPool);
		db.m_channelsAddr = UploadArray(db.m_heap, gpuChannels, device, queue, uploadPool);
		db.m_timesAddr = UploadArray(db.m_heap, allTimes, device, queue, uploadPool);
		db.m_valuesAddr = UploadArray(db.m_heap, allValues, device, queue, uploadPool);
		db.m_nodeParentsAddr = UploadArray(db.m_heap, nodeParents, device, queue, uploadPool);
		db.m_bindTranslationsAddr = UploadArray(db.m_heap, bindTranslations, device, queue, uploadPool);
		db.m_bindRotationsAddr = UploadArray(db.m_heap, bindRotations, device, queue, uploadPool);
		db.m_bindScalesAddr = UploadArray(db.m_heap, bindScales, device, queue, uploadPool);
		db.m_bindTranslations = std::move(bindTranslations);
		db.m_bindRotations = std::move(bindRotations);
		db.m_bindScales = std::move(bindScales);
		db.m_nodeParents = std::move(nodeParents);
		db.m_skinMetas = std::move(skinMetas);
		db.m_skinMetasAddr = UploadArray(db.m_heap, db.m_skinMetas, device, queue, uploadPool);
		db.m_skinJoints = std::move(skinJoints);
		db.m_skinJointsAddr = UploadArray(db.m_heap, db.m_skinJoints, device, queue, uploadPool);
		db.m_skinInverseBinds = std::move(skinInverseBinds);
		db.m_skinInverseBindsAddr = UploadArray(db.m_heap, db.m_skinInverseBinds, device, queue, uploadPool);

		// Log bone hierarchy for debugging
		for (std::uint32_t i = 1; i < asset.nodes.size() && i < 5; ++i)
		{
			const auto& n = asset.nodes[i];
			AE_VERBOSE(LogCategory::Engine, "  Node[{}]: '{}' parent={}, t=({:.1f},{:.1f},{:.1f})", i, n.name, n.parentIndex, n.translation.x, n.translation.y, n.translation.z);
		}
		AE_VERBOSE(LogCategory::Engine, "  Skin count: {}, joints count: {}", asset.skins.size(), asset.skins.empty() ? 0 : asset.skins[0].joints.size());
		if (!asset.skins.empty())
		{
			std::string jointStr;
			for (std::size_t ji = 0; ji < asset.skins[0].joints.size() && ji < 8; ++ji)
			{
				jointStr += std::to_string(asset.skins[0].joints[ji]) + " ";
			}
			AE_VERBOSE(LogCategory::Engine, "  First {} skin joints: {}", (std::min)(asset.skins[0].joints.size(), std::size_t(8)), jointStr);
		}

		AE_VERBOSE(LogCategory::Engine,
		        "AnimationDatabase GPU addresses: clips=0x{:x}, channels=0x{:x}, times=0x{:x}, values=0x{:x}, parents=0x{:x}, bindT=0x{:x}, bindR=0x{:x}, bindS=0x{:x}, skinMetas=0x{:x}, skinJoints=0x{:x}, skinIBMs=0x{:x}, depthNodes=0x{:x}, "
		        "depthRanges=0x{:x}",
		        db.m_clipsAddr,
		        db.m_channelsAddr,
		        db.m_timesAddr,
		        db.m_valuesAddr,
		        db.m_nodeParentsAddr,
		        db.m_bindTranslationsAddr,
		        db.m_bindRotationsAddr,
		        db.m_bindScalesAddr,
		        db.m_skinMetasAddr,
		        db.m_skinJointsAddr,
		        db.m_skinInverseBindsAddr,
		        db.m_depthSortedNodesAddr,
		        db.m_depthRangesAddr);

		if (!db.m_depthSortedNodes.empty())
		{
			db.m_depthSortedNodesAddr = UploadArray(db.m_heap, db.m_depthSortedNodes, device, queue, uploadPool);
			db.m_depthRangesAddr = UploadArray(db.m_heap, db.m_depthRanges, device, queue, uploadPool);
		}

		AE_VERBOSE(LogCategory::Engine,
		        "AnimationDatabase GPU addresses: clips=0x{:x}, channels=0x{:x}, times=0x{:x}, values=0x{:x}, parents=0x{:x}, depthNodes=0x{:x}, depthRanges=0x{:x}",
		        db.m_clipsAddr,
		        db.m_channelsAddr,
		        db.m_timesAddr,
		        db.m_valuesAddr,
		        db.m_nodeParentsAddr,
		        db.m_depthSortedNodesAddr,
		        db.m_depthRangesAddr);

		// Strings: upload as raw bytes using char specialisation.
		if (!allStrings.empty())
		{
			GpuSpan<char> span = db.m_heap.Alloc<char>(static_cast<std::uint32_t>(allStrings.size()));
			db.m_heap.Upload(span, std::span<const char>(allStrings.data(), allStrings.size()), device, queue, static_cast<VkCommandPool>(uploadPool));
			db.m_stringsAddr = span.address;
		}

		// Store CPU copies for AppendAnimations rebuild (after GPU upload so locals are intact).
		db.m_channels = std::move(gpuChannels);
		db.m_times = std::move(allTimes);
		db.m_values = std::move(allValues);

		return std::move(db);
	}

	void AnimationDatabase::Destroy()
	{
		m_heap.Shutdown();
		m_clipsAddr = 0;
		m_channelsAddr = 0;
		m_timesAddr = 0;
		m_valuesAddr = 0;
		m_stringsAddr = 0;
		m_nodeParentsAddr = 0;
		m_bindTranslationsAddr = 0;
		m_bindRotationsAddr = 0;
		m_bindScalesAddr = 0;
		m_skinMetasAddr = 0;
		m_skinJointsAddr = 0;
		m_skinInverseBindsAddr = 0;
		m_depthSortedNodesAddr = 0;
		m_depthRangesAddr = 0;
		m_clips.clear();
		m_channels.clear();
		m_times.clear();
		m_values.clear();
		m_skinMetas.clear();
		m_clipNames.clear();
		m_depthSortedNodes.clear();
		m_depthRanges.clear();
		m_bindTranslations.clear();
		m_bindRotations.clear();
		m_bindScales.clear();
		m_nodeParents.clear();
		m_skinInverseBinds.clear();
		m_skinJoints.clear();
		m_nodeCount = 0;
		m_skinCount = 0;
		m_depthCount = 0;
	}

	Expected<std::uint32_t> AnimationDatabase::AppendAnimations(
	        gpu::CommandPool uploadPool, std::span<const GpuClip> newClips, std::span<const GpuChannel> newChannels, std::span<const float> newTimes, std::span<const glm::vec4> newValues, std::string_view newClipNames)
	{
		if (newClips.empty() || !m_ctx)
		{
			return static_cast<std::uint32_t>(m_clips.size());
		}

		const std::uint32_t firstClipIdx = static_cast<std::uint32_t>(m_clips.size());

		// -- Adjust and append clip data --------------------------------
		const std::uint32_t channelBase = static_cast<std::uint32_t>(m_channels.size());
		const std::uint32_t timesBase = static_cast<std::uint32_t>(m_times.size());
		const std::uint32_t valuesBase = static_cast<std::uint32_t>(m_values.size());
		const std::uint32_t nameBase = static_cast<std::uint32_t>(m_clipNames.size());

		m_clips.reserve(m_clips.size() + newClips.size());
		for (std::size_t i = 0; i < newClips.size(); ++i)
		{
			auto clip = newClips[i];
			clip.channelOffset += channelBase;
			clip.nameOffset += nameBase;
			m_clips.push_back(clip);
		}

		m_channels.reserve(m_channels.size() + newChannels.size());
		for (const auto& ch: newChannels)
		{
			auto gpuCh = ch;
			gpuCh.timesOffset += timesBase * sizeof(float);
			gpuCh.valuesOffset += valuesBase * sizeof(glm::vec4);
			m_channels.push_back(gpuCh);
		}

		m_times.insert(m_times.end(), newTimes.begin(), newTimes.end());
		m_values.insert(m_values.end(), newValues.begin(), newValues.end());
		m_clipNames += newClipNames;

		// -- Rebuild GPU heap with combined data ------------------------
		VkDevice device = m_ctx->GetDevice().device;
		VkQueue queue = m_ctx->GetGraphicsQueue();

		const auto align16 = [](VkDeviceSize v) -> VkDeviceSize
		{
			return (v + 15) & ~VkDeviceSize(15);
		};

		VkDeviceSize totalBytes = align16(m_clips.size() * sizeof(GpuClip));
		totalBytes += align16(m_channels.size() * sizeof(GpuChannel));
		totalBytes += align16(m_times.size() * sizeof(float));
		totalBytes += align16(m_values.size() * sizeof(glm::vec4));
		totalBytes += align16(m_nodeParents.size() * sizeof(std::int32_t));
		totalBytes += align16(m_bindTranslations.size() * sizeof(glm::vec4));
		totalBytes += align16(m_bindRotations.size() * sizeof(glm::vec4));
		totalBytes += align16(m_bindScales.size() * sizeof(glm::vec4));
		totalBytes += align16(m_skinMetas.size() * sizeof(GpuSkinMeta));
		totalBytes += align16(m_skinJoints.size() * sizeof(std::uint32_t));
		totalBytes += align16(m_skinInverseBinds.size() * sizeof(glm::mat4));
		totalBytes += align16(m_depthSortedNodes.size() * sizeof(std::uint32_t));
		totalBytes += align16(m_depthRanges.size() * sizeof(DepthRange));
		totalBytes += align16(m_clipNames.size());

		m_heap.Shutdown();
		m_heap.Initialize(*m_ctx, {.capacityBytes = totalBytes, .debugName = "AnimationDatabase"});

		m_clipsAddr = UploadArray(m_heap, m_clips, device, queue, uploadPool);
		m_channelsAddr = UploadArray(m_heap, m_channels, device, queue, uploadPool);
		m_timesAddr = UploadArray(m_heap, m_times, device, queue, uploadPool);
		m_valuesAddr = UploadArray(m_heap, m_values, device, queue, uploadPool);
		m_nodeParentsAddr = UploadArray(m_heap, m_nodeParents, device, queue, uploadPool);
		m_bindTranslationsAddr = UploadArray(m_heap, m_bindTranslations, device, queue, uploadPool);
		m_bindRotationsAddr = UploadArray(m_heap, m_bindRotations, device, queue, uploadPool);
		m_bindScalesAddr = UploadArray(m_heap, m_bindScales, device, queue, uploadPool);
		m_skinMetasAddr = UploadArray(m_heap, m_skinMetas, device, queue, uploadPool);
		m_skinJointsAddr = UploadArray(m_heap, m_skinJoints, device, queue, uploadPool);
		m_skinInverseBindsAddr = UploadArray(m_heap, m_skinInverseBinds, device, queue, uploadPool);

		if (!m_depthSortedNodes.empty())
		{
			m_depthSortedNodesAddr = UploadArray(m_heap, m_depthSortedNodes, device, queue, uploadPool);
			m_depthRangesAddr = UploadArray(m_heap, m_depthRanges, device, queue, uploadPool);
		}

		if (!m_clipNames.empty())
		{
			GpuSpan<char> span = m_heap.Alloc<char>(static_cast<std::uint32_t>(m_clipNames.size()));
			m_heap.Upload(span, std::span<const char>(m_clipNames.data(), m_clipNames.size()), device, queue, static_cast<VkCommandPool>(uploadPool));
			m_stringsAddr = span.address;
		}

		return firstClipIdx;
	}

	std::string_view AnimationDatabase::GetClipName(std::uint32_t clipIndex) const
	{
		if (clipIndex >= m_clips.size())
		{
			return "";
		}
		const auto& clip = m_clips[clipIndex];
		return std::string_view(m_clipNames.data() + clip.nameOffset, clip.nameLength);
	}
} // namespace aether
