#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "gpu/GpuTypes.hpp"

#include "assets/GltfAsset.hpp"
#include "utils/Expected.hpp"
#include "vulkan/GpuHeap.hpp"

namespace aether
{
	class VulkanContext;

	class AnimationDatabase
	{
	public:
		struct GpuChannel
		{
			std::uint32_t nodeIndex;
			std::uint8_t animPath;
			std::uint8_t interpolation;
			std::uint16_t padding0;
			std::uint32_t timesOffset;
			std::uint32_t timesCount;
			std::uint32_t valuesOffset;
			std::uint32_t valuesCount;
			std::uint64_t padding1;
		};

		static_assert(sizeof(GpuChannel) == 32);

		struct GpuClip
		{
			std::uint32_t nameOffset;
			std::uint32_t nameLength;
			std::uint32_t channelOffset;
			std::uint32_t channelCount;
			float duration;
			std::uint32_t padding0;
		};

		static_assert(sizeof(GpuClip) == 24);

		struct GpuSkinMeta
		{
			std::uint32_t jointOffset;
			std::uint32_t jointCount;
			std::uint32_t inverseBindOffset;
			std::uint32_t _pad0;
		};

		static_assert(sizeof(GpuSkinMeta) == 16);

		struct DepthRange
		{
			std::uint32_t startIndex;
			std::uint32_t count;
		};

		AnimationDatabase() = default;

		~AnimationDatabase()
		{
			++m_generation;
			m_aliveSentinel = 0;
		}

		AnimationDatabase(const AnimationDatabase&) = delete;
		AnimationDatabase& operator=(const AnimationDatabase&) = delete;

		AnimationDatabase(AnimationDatabase&&) noexcept = default;
		AnimationDatabase& operator=(AnimationDatabase&&) noexcept = default;

		static constexpr std::uint32_t kAliveSentinel = 0xABCD1234u;

		[[nodiscard]] bool IsAlive() const
		{
			return m_aliveSentinel == kAliveSentinel;
		}

		// ctx / uploadPool must outlive the Create call (not stored).
		static AnimationDatabase Create(const VulkanContext& ctx, gpu::CommandPool uploadPool, const assets::GltfAsset& asset);

		// The channels must already be remapped to this skeleton's node indices.
		Expected<std::uint32_t> AppendAnimations(
		        gpu::CommandPool uploadPool, std::span<const GpuClip> newClips, std::span<const GpuChannel> newChannels, std::span<const float> newTimes, std::span<const glm::vec4> newValues, std::string_view newClipNames);

		void Destroy();

		[[nodiscard]] gpu::DeviceAddress GetClipsAddr() const
		{
			return m_clipsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetChannelsAddr() const
		{
			return m_channelsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetTimesAddr() const
		{
			return m_timesAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetValuesAddr() const
		{
			return m_valuesAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetStringsAddr() const
		{
			return m_stringsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetNodeParentsAddr() const
		{
			return m_nodeParentsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetBindTranslationsAddr() const
		{
			return m_bindTranslationsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetBindRotationsAddr() const
		{
			return m_bindRotationsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetBindScalesAddr() const
		{
			return m_bindScalesAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetSkinMetasAddr() const
		{
			return m_skinMetasAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetSkinJointsAddr() const
		{
			return m_skinJointsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetSkinInverseBindsAddr() const
		{
			return m_skinInverseBindsAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetDepthSortedNodesAddr() const
		{
			return m_depthSortedNodesAddr;
		}

		[[nodiscard]] gpu::DeviceAddress GetDepthRangesAddr() const
		{
			return m_depthRangesAddr;
		}

		[[nodiscard]] std::uint32_t GetGeneration() const
		{
			return m_generation;
		}

		[[nodiscard]] std::uint32_t GetDepthCount() const
		{
			return m_depthCount;
		}

		[[nodiscard]] const DepthRange& GetDepthRange(std::uint32_t index) const
		{
			return m_depthRanges[index];
		}

		[[nodiscard]] std::uint32_t GetClipCount() const
		{
			return static_cast<std::uint32_t>(m_clips.size());
		}

		[[nodiscard]] std::uint32_t GetNodeCount() const
		{
			return m_nodeCount;
		}

		[[nodiscard]] std::uint32_t GetSkinCount() const
		{
			return m_skinCount;
		}

		[[nodiscard]] std::string_view GetClipName(std::uint32_t clipIndex) const;

		[[nodiscard]] float GetClipDuration(std::uint32_t clipIndex) const
		{
			if (clipIndex >= m_clips.size())
			{
				return 0.f;
			}
			return m_clips[clipIndex].duration;
		}

		[[nodiscard]] std::uint32_t GetSkinJointCount(std::uint32_t skinIndex) const
		{
			if (skinIndex >= m_skinMetas.size())
			{
				return 0u;
			}
			return m_skinMetas[skinIndex].jointCount;
		}

		[[nodiscard]] bool IsValid() const
		{
			return !m_clips.empty();
		}

		[[nodiscard]] const std::vector<glm::vec4>& GetBindTranslations() const
		{
			return m_bindTranslations;
		}

		[[nodiscard]] const std::vector<glm::vec4>& GetBindRotations() const
		{
			return m_bindRotations;
		}

		[[nodiscard]] const std::vector<glm::vec4>& GetBindScales() const
		{
			return m_bindScales;
		}

		[[nodiscard]] const std::vector<std::int32_t>& GetNodeParents() const
		{
			return m_nodeParents;
		}

		[[nodiscard]] const std::vector<std::uint32_t>& GetSkinJoints() const
		{
			return m_skinJoints;
		}

		[[nodiscard]] std::string_view GetNodeName(std::uint32_t nodeIndex) const
		{
			if (nodeIndex >= m_nodeNames.size())
			{
				return {};
			}
			return m_nodeNames[nodeIndex];
		}

		[[nodiscard]] const std::vector<std::string>& GetNodeNames() const
		{
			return m_nodeNames;
		}

	private:
		const VulkanContext* m_ctx = nullptr;

		std::unique_ptr<GpuHeap> m_heap = std::make_unique<GpuHeap>();

		gpu::DeviceAddress m_clipsAddr = 0;
		gpu::DeviceAddress m_channelsAddr = 0;
		gpu::DeviceAddress m_timesAddr = 0;
		gpu::DeviceAddress m_valuesAddr = 0;
		gpu::DeviceAddress m_stringsAddr = 0;
		gpu::DeviceAddress m_nodeParentsAddr = 0;
		gpu::DeviceAddress m_bindTranslationsAddr = 0;
		gpu::DeviceAddress m_bindRotationsAddr = 0;
		gpu::DeviceAddress m_bindScalesAddr = 0;
		gpu::DeviceAddress m_skinMetasAddr = 0;
		gpu::DeviceAddress m_skinJointsAddr = 0;
		gpu::DeviceAddress m_skinInverseBindsAddr = 0;
		gpu::DeviceAddress m_depthSortedNodesAddr = 0;
		gpu::DeviceAddress m_depthRangesAddr = 0;

		std::vector<GpuClip> m_clips;
		std::vector<GpuChannel> m_channels;
		std::vector<float> m_times;
		std::vector<glm::vec4> m_values;
		std::vector<GpuSkinMeta> m_skinMetas;
		std::vector<glm::vec4> m_bindTranslations;
		std::vector<glm::vec4> m_bindRotations;
		std::vector<glm::vec4> m_bindScales;
		std::vector<std::int32_t> m_nodeParents;
		std::vector<glm::mat4> m_skinInverseBinds;
		std::vector<std::uint32_t> m_skinJoints;
		std::string m_clipNames;
		std::vector<std::string> m_nodeNames;
		std::vector<std::uint32_t> m_depthSortedNodes;
		std::vector<DepthRange> m_depthRanges;
		std::uint32_t m_nodeCount = 0;
		std::uint32_t m_skinCount = 0;
		std::uint32_t m_depthCount = 0;
		std::uint32_t m_generation{0};
		std::uint32_t m_aliveSentinel = 0xABCD1234u;
	};
} // namespace aether
