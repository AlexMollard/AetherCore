#include "animation/AnimationDatabase.hpp"

#include <algorithm>

#include <limits>
#include <new>
#include <type_traits>
#include <exception>
#include "utils/Assert.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/TransferManager.hpp"

namespace aether
{
	template<typename T>
	static Expected<gpu::DeviceAddress> UploadArray(GpuHeap& heap, const std::vector<T>& data, vulkan::TransferManager& transfer)
	{
		if (data.empty()) return 0;
		const auto span = heap.Alloc<T>(static_cast<std::uint32_t>(data.size()));
		if (!span.IsValid())
			AE_UNEXPECTED(AetherError::Asset("Animation database GPU allocation failed"));
		AE_TRY(ticket, heap.TryUpload(span, std::span<const T>(data), transfer));
		if (*ticket == 0)
			AE_UNEXPECTED(AetherError::Asset("Animation database GPU upload failed"));
		return span.address;
	}

	AnimationDatabase& AnimationDatabase::operator=(AnimationDatabase&& other) noexcept
	{
		if (this != &other)
		{
			this->~AnimationDatabase();
			new (this) AnimationDatabase(std::move(other));
		}
		return *this;
	}

	AnimationDatabase AnimationDatabase::Create(const VulkanContext& ctx, const assets::GltfAsset& asset)
	{
		auto upload = BeginUpload(ctx, asset);
		if (!upload) Throw(upload.error());
		(*upload)->m_requiredForNextFrame = true;
		for (;;)
		{
			auto progress = (*upload)->Pump(std::numeric_limits<std::size_t>::max());
			if (!progress) Throw(progress.error());
			if (progress->complete) break;
		}
		return (*upload)->Take();
	}

	Expected<std::unique_ptr<AnimationDatabaseUpload>> AnimationDatabase::BeginUpload(const VulkanContext& ctx, const assets::GltfAsset& asset)
	{
		AE_PROFILE_ZONE();
		auto upload = std::unique_ptr<AnimationDatabaseUpload>(new AnimationDatabaseUpload());
		auto& db = upload->m_database;
		db.m_ctx = &ctx;
		db.m_nodeCount = static_cast<std::uint32_t>(asset.nodes.size());
		db.m_skinCount = static_cast<std::uint32_t>(asset.skins.size());

		if (asset.animations.empty())
		{
			return upload;
		}
		const auto numNodes = static_cast<std::uint32_t>(asset.nodes.size());
		if (numNodes == 0)
		{
			// With no nodes, numNodes - 1 below would wrap to UINT32_MAX and the
			// clamp would keep channels whose nodeIndex points past the empty
			// bind-pose arrays the compute skinning pass reads.
			AE_WARN(LogCategory::Engine, "AnimationDatabase: asset has {} animations but no nodes; skipping animation upload.", asset.animations.size());
			return upload;
		}

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

		db.m_channels = std::move(gpuChannels);
		db.m_times = std::move(allTimes);
		db.m_values = std::move(allValues);
		db.m_bindTranslations = std::move(bindTranslations);
		db.m_bindRotations = std::move(bindRotations);
		db.m_bindScales = std::move(bindScales);
		db.m_nodeParents = std::move(nodeParents);
		db.m_skinMetas = std::move(skinMetas);
		db.m_skinJoints = std::move(skinJoints);
		db.m_skinInverseBinds = std::move(skinInverseBinds);

		auto queue = [&](const auto& data, gpu::DeviceAddress& address) -> bool
		{
			using T = typename std::decay_t<decltype(data)>::value_type;
			if (data.empty()) return true;
			if (data.size() > std::numeric_limits<std::uint32_t>::max()) return false;
			const auto span = db.m_heap->Alloc<T>(static_cast<std::uint32_t>(data.size()));
			if (!span.IsValid()) return false;
			address = span.address;
			upload->m_copies.push_back({reinterpret_cast<const std::byte*>(data.data()), data.size() * sizeof(T), span.address});
			return true;
		};
		if (!queue(db.m_clips, db.m_clipsAddr) || !queue(db.m_channels, db.m_channelsAddr) ||
		    !queue(db.m_times, db.m_timesAddr) || !queue(db.m_values, db.m_valuesAddr) ||
		    !queue(db.m_nodeParents, db.m_nodeParentsAddr) ||
		    !queue(db.m_bindTranslations, db.m_bindTranslationsAddr) ||
		    !queue(db.m_bindRotations, db.m_bindRotationsAddr) ||
		    !queue(db.m_bindScales, db.m_bindScalesAddr) ||
		    !queue(db.m_skinMetas, db.m_skinMetasAddr) ||
		    !queue(db.m_skinJoints, db.m_skinJointsAddr) ||
		    !queue(db.m_skinInverseBinds, db.m_skinInverseBindsAddr) ||
		    !queue(db.m_depthSortedNodes, db.m_depthSortedNodesAddr) ||
		    !queue(db.m_depthRanges, db.m_depthRangesAddr) ||
		    !queue(db.m_clipNames, db.m_stringsAddr))
			AE_UNEXPECTED(AetherError::Asset("Animation database GPU allocation failed"));
		return upload;
	}

	AnimationDatabaseUpload::~AnimationDatabaseUpload()
	{
		if (!m_taken && m_ticket && m_database.m_ctx)
			AE_ASSERT_ALWAYS(m_database.m_ctx->GetTransferManager().IsComplete(m_ticket),
			        "Animation upload must be retained until its transfer ticket completes");
	}

	std::size_t AnimationDatabaseUpload::NextChunkSize(std::size_t budget) const
	{
		if (m_taken || m_index == m_copies.size()) return 0;
		const auto remaining = m_copies[m_index].size - m_offset;
		auto count = std::min({remaining, budget, static_cast<std::size_t>(UINT32_MAX)});
		if (count < remaining) count &= ~std::size_t{3};
		return count;
	}

	Expected<std::uint64_t> AnimationDatabaseUpload::NextStagingRequirement(std::size_t budget) const
	{
		if (m_taken) AE_UNEXPECTED(AetherError::Asset("Animation upload already taken"));
		const auto count = NextChunkSize(budget);
		if (!count) return 0;
		gpu::MappedBufferDesc desc{};
		desc.size = count;
		desc.usage = gpu::BufferUsage::TransferSrc;
		const auto requirement = gpu::ResourceRegistry::GetMappedBufferAllocationRequirement(desc);
		if (!requirement) AE_UNEXPECTED(AetherError::Asset("Cannot query animation staging allocation"));
		return requirement;
	}

	Expected<AnimationUploadProgress> AnimationDatabaseUpload::Pump(std::size_t budget, std::function<void()> onStagingRetired)
	{
		if (m_taken)
		{
			if (onStagingRetired) onStagingRetired();
			AE_UNEXPECTED(AetherError::Asset("Animation upload already taken"));
		}
		AnimationUploadProgress result;
		auto& transfer = m_database.m_ctx->GetTransferManager();
		const auto count = NextChunkSize(budget);
		if (!count && onStagingRetired) onStagingRetired();
		if (count)
		{
			const auto& copy = m_copies[m_index];
			GpuSpan<std::byte> destination{};
			destination.address = copy.address + m_offset;
			destination.count = static_cast<std::uint32_t>(count);
			const auto ticket = m_database.m_heap->Upload(destination,
			        std::span<const std::byte>(copy.data + m_offset, count), transfer, m_requiredForNextFrame, std::move(onStagingRetired));
			if (!ticket)
				AE_UNEXPECTED(AetherError::Asset("Animation database GPU upload failed"));
			m_ticket = std::max(m_ticket, ticket);
			result.bytes += count;
			m_offset += count;
			if (m_offset == copy.size)
			{
				++m_index;
				m_offset = 0;
			}
		}
		result.ticket = m_ticket;
		result.complete = m_index == m_copies.size();
		return result;
	}

	AnimationDatabase AnimationDatabaseUpload::Take()
	{
		if (m_taken || m_index != m_copies.size())
			Throw(AetherError::Asset("Animation upload is not complete"));
		m_taken = true;
		return std::move(m_database);
	}

	void AnimationDatabase::Destroy(std::function<void()> onRetired)
	{
		AE_PROFILE_ZONE();
		if (m_heap) m_heap->ShutdownDeferred(std::move(onRetired));
		else if (onRetired) onRetired();
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

		// Build privately: allocation failure must not change CPU counts or published addresses.
		AnimationDatabase replacement;
		replacement.m_clips = m_clips;
		replacement.m_channels = m_channels;
		replacement.m_times = m_times;
		replacement.m_values = m_values;
		replacement.m_skinMetas = m_skinMetas;
		replacement.m_bindTranslations = m_bindTranslations;
		replacement.m_bindRotations = m_bindRotations;
		replacement.m_bindScales = m_bindScales;
		replacement.m_nodeParents = m_nodeParents;
		replacement.m_skinInverseBinds = m_skinInverseBinds;
		replacement.m_skinJoints = m_skinJoints;
		replacement.m_clipNames = m_clipNames;
		replacement.m_nodeNames = m_nodeNames;
		replacement.m_depthSortedNodes = m_depthSortedNodes;
		replacement.m_depthRanges = m_depthRanges;
		replacement.m_nodeCount = m_nodeCount;
		replacement.m_skinCount = m_skinCount;
		replacement.m_depthCount = m_depthCount;
		replacement.m_generation = m_generation;
		replacement.m_aliveSentinel = m_aliveSentinel;
		replacement.m_ctx = m_ctx;
		auto& transfer = m_ctx->GetTransferManager();
		struct RetireReplacement
		{
			GpuHeap& heap;
			vulkan::TransferManager& transfer;
			bool active = true;
			~RetireReplacement()
			{
				if (!active) { return; }
				// Some earlier copies may already have been submitted when a later staging
				// allocation fails. Never destroy their destination while DMA still uses it.
				// WaitFor fails fatally on device loss rather than resuming with unsafe memory.
				transfer.WaitFor(transfer.LastSubmitted());
				heap.Shutdown();
			}
		} retire{*replacement.m_heap, transfer};
		const auto firstClipIdx = static_cast<std::uint32_t>(replacement.m_clips.size());

		const auto channelBase = static_cast<std::uint32_t>(replacement.m_channels.size());
		const auto timesBase = static_cast<std::uint32_t>(replacement.m_times.size());
		const auto valuesBase = static_cast<std::uint32_t>(replacement.m_values.size());
		const auto nameBase = static_cast<std::uint32_t>(replacement.m_clipNames.size());

		replacement.m_clips.reserve(replacement.m_clips.size() + newClips.size());
		for (auto clip: newClips)
		{
			clip.channelOffset += channelBase;
			clip.nameOffset += nameBase;
			replacement.m_clips.push_back(clip);
		}

		replacement.m_channels.reserve(replacement.m_channels.size() + newChannels.size());
		for (const auto& ch: newChannels)
		{
			auto gpuCh = ch;
			gpuCh.timesOffset += timesBase * sizeof(float);
			gpuCh.valuesOffset += valuesBase * sizeof(glm::vec4);
			replacement.m_channels.push_back(gpuCh);
		}

		replacement.m_times.insert(replacement.m_times.end(), newTimes.begin(), newTimes.end());
		replacement.m_values.insert(replacement.m_values.end(), newValues.begin(), newValues.end());
		replacement.m_clipNames += newClipNames;


		const auto align16 = [](gpu::DeviceSize v) -> gpu::DeviceSize
		{
			return (v + 15) & ~static_cast<gpu::DeviceSize>(15);
		};
		gpu::DeviceSize totalBytes = align16(replacement.m_clips.size() * sizeof(GpuClip));
		totalBytes += align16(replacement.m_channels.size() * sizeof(GpuChannel));
		totalBytes += align16(replacement.m_times.size() * sizeof(float));
		totalBytes += align16(replacement.m_values.size() * sizeof(glm::vec4));
		totalBytes += align16(replacement.m_nodeParents.size() * sizeof(std::int32_t));
		totalBytes += align16(replacement.m_bindTranslations.size() * sizeof(glm::vec4));
		totalBytes += align16(replacement.m_bindRotations.size() * sizeof(glm::vec4));
		totalBytes += align16(replacement.m_bindScales.size() * sizeof(glm::vec4));
		totalBytes += align16(replacement.m_skinMetas.size() * sizeof(GpuSkinMeta));
		totalBytes += align16(replacement.m_skinJoints.size() * sizeof(std::uint32_t));
		totalBytes += align16(replacement.m_skinInverseBinds.size() * sizeof(glm::mat4));
		totalBytes += align16(replacement.m_depthSortedNodes.size() * sizeof(std::uint32_t));
		totalBytes += align16(replacement.m_depthRanges.size() * sizeof(DepthRange));
		totalBytes += align16(replacement.m_clipNames.size());

		auto initialized = replacement.m_heap->TryInitialize(*replacement.m_ctx, {.capacityBytes = totalBytes, .debugName = "AnimationDatabase"});
		if (!initialized) { return std::unexpected(initialized.error()); }

		AE_TRY(clipsAddr, UploadArray(*replacement.m_heap, replacement.m_clips, transfer));
		replacement.m_clipsAddr = *clipsAddr;
		AE_TRY(channelsAddr, UploadArray(*replacement.m_heap, replacement.m_channels, transfer));
		replacement.m_channelsAddr = *channelsAddr;
		AE_TRY(timesAddr, UploadArray(*replacement.m_heap, replacement.m_times, transfer));
		replacement.m_timesAddr = *timesAddr;
		AE_TRY(valuesAddr, UploadArray(*replacement.m_heap, replacement.m_values, transfer));
		replacement.m_valuesAddr = *valuesAddr;
		AE_TRY(nodeParentsAddr, UploadArray(*replacement.m_heap, replacement.m_nodeParents, transfer));
		replacement.m_nodeParentsAddr = *nodeParentsAddr;
		AE_TRY(bindTranslationsAddr, UploadArray(*replacement.m_heap, replacement.m_bindTranslations, transfer));
		replacement.m_bindTranslationsAddr = *bindTranslationsAddr;
		AE_TRY(bindRotationsAddr, UploadArray(*replacement.m_heap, replacement.m_bindRotations, transfer));
		replacement.m_bindRotationsAddr = *bindRotationsAddr;
		AE_TRY(bindScalesAddr, UploadArray(*replacement.m_heap, replacement.m_bindScales, transfer));
		replacement.m_bindScalesAddr = *bindScalesAddr;
		AE_TRY(skinMetasAddr, UploadArray(*replacement.m_heap, replacement.m_skinMetas, transfer));
		replacement.m_skinMetasAddr = *skinMetasAddr;
		AE_TRY(skinJointsAddr, UploadArray(*replacement.m_heap, replacement.m_skinJoints, transfer));
		replacement.m_skinJointsAddr = *skinJointsAddr;
		AE_TRY(skinInverseBindsAddr, UploadArray(*replacement.m_heap, replacement.m_skinInverseBinds, transfer));
		replacement.m_skinInverseBindsAddr = *skinInverseBindsAddr;

		if (!replacement.m_depthSortedNodes.empty())
		{
			AE_TRY(depthSortedNodesAddr, UploadArray(*replacement.m_heap, replacement.m_depthSortedNodes, transfer));
			replacement.m_depthSortedNodesAddr = *depthSortedNodesAddr;
			AE_TRY(depthRangesAddr, UploadArray(*replacement.m_heap, replacement.m_depthRanges, transfer));
			replacement.m_depthRangesAddr = *depthRangesAddr;
		}

		if (!replacement.m_clipNames.empty())
		{
			const GpuSpan<char> span = replacement.m_heap->Alloc<char>(static_cast<std::uint32_t>(replacement.m_clipNames.size()));
			if (!span.IsValid()) { AE_UNEXPECTED(AetherError::Asset("Animation strings GPU allocation failed")); }
			AE_TRY(ticket, replacement.m_heap->TryUpload(span, std::span<const char>(replacement.m_clipNames.data(), replacement.m_clipNames.size()), transfer));
			if (*ticket == 0) { AE_UNEXPECTED(AetherError::Asset("Animation strings GPU upload failed")); }
			replacement.m_stringsAddr = span.address;
		}

		// All fallible work is complete before publishing. The exclusive caller drained
		// old frames and waited for GPU idle, so the old heap can be released directly.
		transfer.WaitFor(transfer.LastSubmitted());
		retire.active = false;
		m_heap->Shutdown();
		m_heap.swap(replacement.m_heap);
		m_clips.swap(replacement.m_clips);
		m_channels.swap(replacement.m_channels);
		m_times.swap(replacement.m_times);
		m_values.swap(replacement.m_values);
		m_skinMetas.swap(replacement.m_skinMetas);
		m_bindTranslations.swap(replacement.m_bindTranslations);
		m_bindRotations.swap(replacement.m_bindRotations);
		m_bindScales.swap(replacement.m_bindScales);
		m_nodeParents.swap(replacement.m_nodeParents);
		m_skinInverseBinds.swap(replacement.m_skinInverseBinds);
		m_skinJoints.swap(replacement.m_skinJoints);
		m_clipNames.swap(replacement.m_clipNames);
		m_nodeNames.swap(replacement.m_nodeNames);
		m_depthSortedNodes.swap(replacement.m_depthSortedNodes);
		m_depthRanges.swap(replacement.m_depthRanges);
		m_clipsAddr = replacement.m_clipsAddr;
		m_channelsAddr = replacement.m_channelsAddr;
		m_timesAddr = replacement.m_timesAddr;
		m_valuesAddr = replacement.m_valuesAddr;
		m_stringsAddr = replacement.m_stringsAddr;
		m_nodeParentsAddr = replacement.m_nodeParentsAddr;
		m_bindTranslationsAddr = replacement.m_bindTranslationsAddr;
		m_bindRotationsAddr = replacement.m_bindRotationsAddr;
		m_bindScalesAddr = replacement.m_bindScalesAddr;
		m_skinMetasAddr = replacement.m_skinMetasAddr;
		m_skinJointsAddr = replacement.m_skinJointsAddr;
		m_skinInverseBindsAddr = replacement.m_skinInverseBindsAddr;
		m_depthSortedNodesAddr = replacement.m_depthSortedNodesAddr;
		m_depthRangesAddr = replacement.m_depthRangesAddr;

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
