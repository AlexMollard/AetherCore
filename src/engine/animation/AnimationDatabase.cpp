#include "animation/AnimationDatabase.hpp"

#include <algorithm>

#include "gpu/GpuTypes.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/TransferManager.hpp"

namespace aether
{
	// Uploads go through the transfer queue as tickets; the frame submission's timeline
	// wait orders them before any GPU consumption, so nothing here blocks. The source
	// vector is memcpy'd into staging before Submit returns, so locals are safe.
	template<typename T>
	static gpu::DeviceAddress UploadArray(GpuHeap& heap, const std::vector<T>& data, vulkan::TransferManager& transfer)
	{
		if (data.empty())
		{
			return 0;
		}
		const GpuSpan<T> span = heap.Alloc<T>(static_cast<std::uint32_t>(data.size()));
		assert(span.IsValid() && "GpuHeap allocation failed - heap capacity insufficient");
		(void) heap.Upload(span, std::span<const T>(data), transfer);
		return span.address;
	}

	AnimationDatabase AnimationDatabase::Create(const VulkanContext& ctx, const assets::GltfAsset& asset)
	{
		AE_PROFILE_ZONE();
		AnimationDatabase db;
		db.m_ctx = &ctx;
		db.m_nodeCount = static_cast<std::uint32_t>(asset.nodes.size());
		db.m_skinCount = static_cast<std::uint32_t>(asset.skins.size());

		if (asset.animations.empty())
		{
			return db;
		}

		const auto numNodes = static_cast<std::uint32_t>(asset.nodes.size());
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
		std::string allStrings;

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

		db.m_nodeNames.reserve(asset.nodes.size());
		for (const auto& n: asset.nodes)
		{
			db.m_nodeNames.push_back(n.name);
		}

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

		// GpuHeap::AllocBytes enforces a 16-byte minimum alignment, so each term
		const auto align16 = [](gpu::DeviceSize v) -> gpu::DeviceSize
		{
			return (v + 15) & ~static_cast<gpu::DeviceSize>(15);
		};
		gpu::DeviceSize totalBytes = align16(gpuClips.size() * sizeof(GpuClip));
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

		db.m_heap->Initialize(ctx, {.capacityBytes = totalBytes, .debugName = "AnimationDatabase"});

		vulkan::TransferManager& transfer = ctx.GetTransferManager();

		db.m_clipsAddr = UploadArray(*db.m_heap, gpuClips, transfer);
		db.m_channelsAddr = UploadArray(*db.m_heap, gpuChannels, transfer);
		db.m_timesAddr = UploadArray(*db.m_heap, allTimes, transfer);
		db.m_valuesAddr = UploadArray(*db.m_heap, allValues, transfer);
		db.m_nodeParentsAddr = UploadArray(*db.m_heap, nodeParents, transfer);
		db.m_bindTranslationsAddr = UploadArray(*db.m_heap, bindTranslations, transfer);
		db.m_bindRotationsAddr = UploadArray(*db.m_heap, bindRotations, transfer);
		db.m_bindScalesAddr = UploadArray(*db.m_heap, bindScales, transfer);
		db.m_bindTranslations = std::move(bindTranslations);
		db.m_bindRotations = std::move(bindRotations);
		db.m_bindScales = std::move(bindScales);
		db.m_nodeParents = std::move(nodeParents);
		db.m_skinMetas = std::move(skinMetas);
		db.m_skinMetasAddr = UploadArray(*db.m_heap, db.m_skinMetas, transfer);
		db.m_skinJoints = std::move(skinJoints);
		db.m_skinJointsAddr = UploadArray(*db.m_heap, db.m_skinJoints, transfer);
		db.m_skinInverseBinds = std::move(skinInverseBinds);
		db.m_skinInverseBindsAddr = UploadArray(*db.m_heap, db.m_skinInverseBinds, transfer);

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
			AE_VERBOSE(LogCategory::Engine, "  First {} skin joints: {}", (std::min) (asset.skins[0].joints.size(), std::size_t(8)), jointStr);
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
			db.m_depthSortedNodesAddr = UploadArray(*db.m_heap, db.m_depthSortedNodes, transfer);
			db.m_depthRangesAddr = UploadArray(*db.m_heap, db.m_depthRanges, transfer);
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

		if (!allStrings.empty())
		{
			const GpuSpan<char> span = db.m_heap->Alloc<char>(static_cast<std::uint32_t>(allStrings.size()));
			(void) db.m_heap->Upload(span, std::span<const char>(allStrings.data(), allStrings.size()), transfer);
			db.m_stringsAddr = span.address;
		}

		db.m_channels = std::move(gpuChannels);
		db.m_times = std::move(allTimes);
		db.m_values = std::move(allValues);

		return db;
	}

	void AnimationDatabase::Destroy()
	{
		AE_PROFILE_ZONE();
		m_heap->Shutdown();
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
	        std::span<const GpuClip> newClips, std::span<const GpuChannel> newChannels, std::span<const float> newTimes, std::span<const glm::vec4> newValues, std::string_view newClipNames)
	{
		AE_PROFILE_ZONE();
		if (newClips.empty() || !m_ctx)
		{
			return static_cast<std::uint32_t>(m_clips.size());
		}

		const auto firstClipIdx = static_cast<std::uint32_t>(m_clips.size());

		const auto channelBase = static_cast<std::uint32_t>(m_channels.size());
		const auto timesBase = static_cast<std::uint32_t>(m_times.size());
		const auto valuesBase = static_cast<std::uint32_t>(m_values.size());
		const auto nameBase = static_cast<std::uint32_t>(m_clipNames.size());

		m_clips.reserve(m_clips.size() + newClips.size());
		for (auto clip: newClips)
		{
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

		vulkan::TransferManager& transfer = m_ctx->GetTransferManager();

		const auto align16 = [](gpu::DeviceSize v) -> gpu::DeviceSize
		{
			return (v + 15) & ~static_cast<gpu::DeviceSize>(15);
		};
		gpu::DeviceSize totalBytes = align16(m_clips.size() * sizeof(GpuClip));
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

		m_heap->Shutdown();
		m_heap->Initialize(*m_ctx, {.capacityBytes = totalBytes, .debugName = "AnimationDatabase"});

		m_clipsAddr = UploadArray(*m_heap, m_clips, transfer);
		m_channelsAddr = UploadArray(*m_heap, m_channels, transfer);
		m_timesAddr = UploadArray(*m_heap, m_times, transfer);
		m_valuesAddr = UploadArray(*m_heap, m_values, transfer);
		m_nodeParentsAddr = UploadArray(*m_heap, m_nodeParents, transfer);
		m_bindTranslationsAddr = UploadArray(*m_heap, m_bindTranslations, transfer);
		m_bindRotationsAddr = UploadArray(*m_heap, m_bindRotations, transfer);
		m_bindScalesAddr = UploadArray(*m_heap, m_bindScales, transfer);
		m_skinMetasAddr = UploadArray(*m_heap, m_skinMetas, transfer);
		m_skinJointsAddr = UploadArray(*m_heap, m_skinJoints, transfer);
		m_skinInverseBindsAddr = UploadArray(*m_heap, m_skinInverseBinds, transfer);

		if (!m_depthSortedNodes.empty())
		{
			m_depthSortedNodesAddr = UploadArray(*m_heap, m_depthSortedNodes, transfer);
			m_depthRangesAddr = UploadArray(*m_heap, m_depthRanges, transfer);
		}

		if (!m_clipNames.empty())
		{
			const GpuSpan<char> span = m_heap->Alloc<char>(static_cast<std::uint32_t>(m_clipNames.size()));
			(void) m_heap->Upload(span, std::span<const char>(m_clipNames.data(), m_clipNames.size()), transfer);
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
