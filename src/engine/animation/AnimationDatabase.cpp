#include "animation/AnimationDatabase.hpp"

#include <algorithm>

#include "vulkan/VulkanContext.hpp"

namespace aether
{
	// Helper: upload a non-empty CPU array into the heap and return its device address.
	// Returns 0 when the input span is empty (no allocation made).
	template<typename T>
	static VkDeviceAddress UploadArray(GpuHeap& heap, const std::vector<T>& data, VkDevice device, VkQueue queue, VkCommandPool pool)
	{
		if (data.empty())
		{
			return 0;
		}
		GpuSpan<T> span = heap.Alloc<T>(static_cast<std::uint32_t>(data.size()));
		assert(span.IsValid() && "GpuHeap allocation failed - heap capacity insufficient");
		heap.Upload(span, std::span<const T>(data), device, queue, pool);
		return span.address;
	}

	AnimationDatabase AnimationDatabase::Create(const VulkanContext& ctx, VkCommandPool uploadPool, const assets::GltfAsset& asset)
	{
		AnimationDatabase db;
		db.m_nodeCount = static_cast<std::uint32_t>(asset.nodes.size());
		db.m_skinCount = static_cast<std::uint32_t>(asset.skins.size());

		if (asset.animations.empty())
		{
			return db;
		}

		// ── Build CPU arrays ─────────────────────────────────────────────────
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
				gpuCh.nodeIndex = srcCh.nodeIndex;
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
		for (const auto& n: asset.nodes)
		{
			nodeParents.push_back(n.parentIndex);
			bindTranslations.emplace_back(n.translation, 0.0f);
			bindRotations.emplace_back(n.rotation.x, n.rotation.y, n.rotation.z, n.rotation.w);
			bindScales.emplace_back(n.scale, 0.0f);
		}

		// ── Compute node depths for level-by-level flatten ──────────────────
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
			nodesAtDepth[nodeDepth[i]].push_back(static_cast<std::uint32_t>(i));
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

		// ── Size the heap and upload all arrays ──────────────────────────────
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
		db.m_skinMetas = skinMetas;
		db.m_skinMetasAddr = UploadArray(db.m_heap, skinMetas, device, queue, uploadPool);
		db.m_skinJoints = std::move(skinJoints);
		db.m_skinJointsAddr = UploadArray(db.m_heap, db.m_skinJoints, device, queue, uploadPool);
		db.m_skinInverseBinds = std::move(skinInverseBinds);
		db.m_skinInverseBindsAddr = UploadArray(db.m_heap, db.m_skinInverseBinds, device, queue, uploadPool);

		if (!db.m_depthSortedNodes.empty())
		{
			db.m_depthSortedNodesAddr = UploadArray(db.m_heap, db.m_depthSortedNodes, device, queue, uploadPool);
			db.m_depthRangesAddr = UploadArray(db.m_heap, db.m_depthRanges, device, queue, uploadPool);
		}

		// Strings: upload as raw bytes using char specialisation.
		if (!allStrings.empty())
		{
			GpuSpan<char> span = db.m_heap.Alloc<char>(static_cast<std::uint32_t>(allStrings.size()));
			db.m_heap.Upload(span, std::span<const char>(allStrings.data(), allStrings.size()), device, queue, uploadPool);
			db.m_stringsAddr = span.address;
		}

		return db;
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
		m_skinMetas.clear();
		m_clipNames.clear();
		m_depthSortedNodes.clear();
		m_depthRanges.clear();
		m_nodeCount = 0;
		m_skinCount = 0;
		m_depthCount = 0;
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
