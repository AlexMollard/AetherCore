#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string_view>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "assets/GltfAsset.hpp"
#include "UniqueBuffer.hpp"

namespace aether
{
	// GPU-friendly animation database.
	// Flattens all animation clip data (keyframes, times, channels) into dense GPU buffers
	// and maintains lookup tables for efficient per-frame sampling on compute shader.
	//
	// Data layout (all on GPU):
	//   - Times buffer: flat array of all keyframe times (float)
	//   - Values buffer: flat array of all keyframe values (vec4)
	//   - Channels buffer: per-channel metadata with offsets into times/values
	//   - Clips buffer: per-clip metadata (name, channel range, duration)
	//   - Strings buffer: animation clip names (stored as UTF-8 with offsets)
	//
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

		AnimationDatabase() = default;
		~AnimationDatabase() = default;

		AnimationDatabase(const AnimationDatabase&) = delete;
		AnimationDatabase& operator=(const AnimationDatabase&) = delete;

		AnimationDatabase(AnimationDatabase&&) noexcept = default;
		AnimationDatabase& operator=(AnimationDatabase&&) noexcept = default;

		// Build database from a glTF asset's animation collection.
		// device/allocator must outlive this object.
		static AnimationDatabase Create(VkDevice device, VmaAllocator allocator, const assets::GltfAsset& asset);

		void Destroy();

		// GPU buffer addresses (device addressable).
		[[nodiscard]] VkDeviceAddress GetClipsAddr() const;
		[[nodiscard]] VkDeviceAddress GetChannelsAddr() const;
		[[nodiscard]] VkDeviceAddress GetTimesAddr() const;
		[[nodiscard]] VkDeviceAddress GetValuesAddr() const;
		[[nodiscard]] VkDeviceAddress GetStringsAddr() const;
		[[nodiscard]] VkDeviceAddress GetNodeParentsAddr() const;
		[[nodiscard]] VkDeviceAddress GetBindTranslationsAddr() const;
		[[nodiscard]] VkDeviceAddress GetBindRotationsAddr() const;
		[[nodiscard]] VkDeviceAddress GetBindScalesAddr() const;
		[[nodiscard]] VkDeviceAddress GetSkinMetasAddr() const;
		[[nodiscard]] VkDeviceAddress GetSkinJointsAddr() const;
		[[nodiscard]] VkDeviceAddress GetSkinInverseBindsAddr() const;

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

		[[nodiscard]] bool IsValid() const
		{
			return !m_clips.empty();
		}

	private:
		UniqueBuffer m_clipsBuffer;
		UniqueBuffer m_channelsBuffer;
		UniqueBuffer m_timesBuffer;
		UniqueBuffer m_valuesBuffer;
		UniqueBuffer m_stringsBuffer;
		UniqueBuffer m_nodeParentsBuffer;
		UniqueBuffer m_bindTranslationsBuffer;
		UniqueBuffer m_bindRotationsBuffer;
		UniqueBuffer m_bindScalesBuffer;
		UniqueBuffer m_skinMetasBuffer;
		UniqueBuffer m_skinJointsBuffer;
		UniqueBuffer m_skinInverseBindsBuffer;

		std::vector<GpuClip> m_clips; // CPU-side copy for debugging
		std::string m_clipNames;      // Concatenated clip name strings
		std::uint32_t m_nodeCount = 0;
		std::uint32_t m_skinCount = 0;

		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
	};
} // namespace aether
