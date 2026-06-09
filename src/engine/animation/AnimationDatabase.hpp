#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>
#include "vulkan/volk.hpp"

#include "assets/GltfAsset.hpp"
#include "utils/Expected.hpp"
#include "vulkan/GpuHeap.hpp"

namespace aether
{
	class VulkanContext;

	// GPU-friendly animation database.
	// Flattens all animation clip data (keyframes, times, channels) into dense GPU buffers
	// and maintains lookup tables for efficient per-frame sampling on compute shader.
	//
	// All data lives in a single device-local GpuHeap (one VkBuffer per database).
	// Individual array addresses are exposed as VkDeviceAddress for push-constant use.
	class AnimationDatabase
	{
	public:
		struct GpuChannel
		{
			std::uint32_t nodeIndex;
			std::uint8_t animPath;      // GltfAnimationPath
			std::uint8_t interpolation; // GltfInterpolation
			std::uint16_t padding0;
			std::uint32_t timesOffset; // byte offset into times buffer
			std::uint32_t timesCount;
			std::uint32_t valuesOffset; // byte offset into values buffer
			std::uint32_t valuesCount;
			std::uint64_t padding1; // Align to 32 bytes
		};

		static_assert(sizeof(GpuChannel) == 32);

		struct GpuClip
		{
			std::uint32_t nameOffset; // byte offset into strings buffer
			std::uint32_t nameLength;
			std::uint32_t channelOffset; // index into channels buffer
			std::uint32_t channelCount;
			float duration;
			std::uint32_t padding0;
		};

		static_assert(sizeof(GpuClip) == 24);

		struct GpuSkinMeta
		{
			std::uint32_t jointOffset;
			std::uint32_t jointCount;
			std::uint32_t inverseBindOffset; // byte offset into inverse-bind buffer
			std::uint32_t _pad0;
		};

		static_assert(sizeof(GpuSkinMeta) == 16);

		struct DepthRange
		{
			std::uint32_t startIndex; // index into depthSortedNodes
			std::uint32_t count;      // nodes at this depth
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

		// Build database from a glTF asset's animation collection.
		// ctx / uploadPool must outlive the Create call (not stored).
		static AnimationDatabase Create(const VulkanContext& ctx, VkCommandPool uploadPool, const assets::GltfAsset& asset);

		// Append runtime-loaded animation clips to this database.
		// The channels must already be remapped to this skeleton's node indices.
		// Destroys and recreates the GPU heap to accommodate the combined data.
		// Returns the clip index of the first newly-added clip.
		// uploadPool must be a valid compute-capable command pool.
		Expected<std::uint32_t> AppendAnimations(
		        VkCommandPool uploadPool, std::span<const GpuClip> newClips, std::span<const GpuChannel> newChannels, std::span<const float> newTimes, std::span<const glm::vec4> newValues, std::string_view newClipNames);

		void Destroy();

		// GPU buffer addresses (device addressable).
		[[nodiscard]] VkDeviceAddress GetClipsAddr() const
		{
			return m_clipsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetChannelsAddr() const
		{
			return m_channelsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetTimesAddr() const
		{
			return m_timesAddr;
		}

		[[nodiscard]] VkDeviceAddress GetValuesAddr() const
		{
			return m_valuesAddr;
		}

		[[nodiscard]] VkDeviceAddress GetStringsAddr() const
		{
			return m_stringsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetNodeParentsAddr() const
		{
			return m_nodeParentsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetBindTranslationsAddr() const
		{
			return m_bindTranslationsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetBindRotationsAddr() const
		{
			return m_bindRotationsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetBindScalesAddr() const
		{
			return m_bindScalesAddr;
		}

		[[nodiscard]] VkDeviceAddress GetSkinMetasAddr() const
		{
			return m_skinMetasAddr;
		}

		[[nodiscard]] VkDeviceAddress GetSkinJointsAddr() const
		{
			return m_skinJointsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetSkinInverseBindsAddr() const
		{
			return m_skinInverseBindsAddr;
		}

		[[nodiscard]] VkDeviceAddress GetDepthSortedNodesAddr() const
		{
			return m_depthSortedNodesAddr;
		}

		[[nodiscard]] VkDeviceAddress GetDepthRangesAddr() const
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

		// CPU accessors for validation/debugging.
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

		// CPU accessors for bind pose data (debug/validation).
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

		[[nodiscard]] const std::vector<glm::mat4>& GetSkinInverseBinds() const
		{
			return m_skinInverseBinds;
		}

		[[nodiscard]] const std::vector<std::uint32_t>& GetSkinJoints() const
		{
			return m_skinJoints;
		}

		[[nodiscard]] const std::vector<GpuSkinMeta>& GetSkinMetas() const
		{
			return m_skinMetas;
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

		GpuHeap m_heap;

		VkDeviceAddress m_clipsAddr = 0;
		VkDeviceAddress m_channelsAddr = 0;
		VkDeviceAddress m_timesAddr = 0;
		VkDeviceAddress m_valuesAddr = 0;
		VkDeviceAddress m_stringsAddr = 0;
		VkDeviceAddress m_nodeParentsAddr = 0;
		VkDeviceAddress m_bindTranslationsAddr = 0;
		VkDeviceAddress m_bindRotationsAddr = 0;
		VkDeviceAddress m_bindScalesAddr = 0;
		VkDeviceAddress m_skinMetasAddr = 0;
		VkDeviceAddress m_skinJointsAddr = 0;
		VkDeviceAddress m_skinInverseBindsAddr = 0;
		VkDeviceAddress m_depthSortedNodesAddr = 0;
		VkDeviceAddress m_depthRangesAddr = 0;

		std::vector<GpuClip> m_clips;              // CPU-side copy for GetClipName()/GetClipDuration()
		std::vector<GpuChannel> m_channels;        // CPU-side copy for AppendAnimations rebuild
		std::vector<float> m_times;                // CPU-side copy for AppendAnimations rebuild
		std::vector<glm::vec4> m_values;           // CPU-side copy for AppendAnimations rebuild
		std::vector<GpuSkinMeta> m_skinMetas;      // CPU-side copy for GetSkinJointCount()
		std::vector<glm::vec4> m_bindTranslations; // CPU-side copy for debug
		std::vector<glm::vec4> m_bindRotations;    // CPU-side copy for debug
		std::vector<glm::vec4> m_bindScales;       // CPU-side copy for debug
		std::vector<std::int32_t> m_nodeParents;   // CPU-side copy for debug
		std::vector<glm::mat4> m_skinInverseBinds; // CPU-side copy for debug
		std::vector<std::uint32_t> m_skinJoints;   // CPU-side copy for debug
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
