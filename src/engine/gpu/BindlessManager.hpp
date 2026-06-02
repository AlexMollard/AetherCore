#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "vulkan/volk.hpp"

#include "material/BindlessContract.hpp"
#include "utils/Assert.hpp"

namespace aether
{
	class VulkanContext;

	class BindlessManager
	{
	public:
		struct Config
		{
			std::uint32_t maxSampledImages = 4096;
			std::uint32_t deferredFreeFrames = 3;
		};

		struct SamplerKey
		{
			VkFilter filter = VK_FILTER_LINEAR;
			VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			bool operator==(const SamplerKey& other) const = default;
		};

		struct SamplerKeyHash
		{
			std::size_t operator()(const SamplerKey& key) const
			{
				std::size_t h = static_cast<std::size_t>(key.filter);
				h ^= static_cast<std::size_t>(key.mipmapMode) << 4;
				h ^= static_cast<std::size_t>(key.addressMode) << 8;
				return h;
			}
		};

		BindlessManager() = default;
		~BindlessManager();

		BindlessManager(const BindlessManager&) = AE_DELETE_MSG("BindlessManager manages GPU descriptor resources - use reference");
		BindlessManager& operator=(const BindlessManager&) = AE_DELETE_MSG("BindlessManager manages GPU descriptor resources - use reference");

		Expected<void> Initialize(const VulkanContext& context, const Config& config);
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const;
		[[nodiscard]] VkDescriptorSetLayout GetLayout() const;
		[[nodiscard]] VkDescriptorSet GetSet() const;
		[[nodiscard]] std::uint32_t GetCapacity() const;
		[[nodiscard]] std::uint64_t GetCurrentFrame() const;

		[[nodiscard]] static constexpr std::uint32_t GetDescriptorSetIndex()
		{
			return bindless::kDescriptorSetIndex;
		}

		[[nodiscard]] static constexpr std::uint32_t GetSampledImageBinding()
		{
			return bindless::kSampledImageBinding;
		}

		[[nodiscard]] static constexpr std::uint32_t GetLinearSamplerBinding()
		{
			return 1u;
		}

		// Returns a cached VkSampler matching the given filter/mipmap/address mode.
		// Creates a new sampler if no matching one exists. Thread-safe.
		[[nodiscard]] Expected<VkSampler> GetOrCreateSampler(VkFilter filter, VkSamplerMipmapMode mipmapMode, VkSamplerAddressMode addressMode);

		[[nodiscard]] Expected<std::uint32_t> AllocateSampledImageSlot();
		void FreeSampledImageSlot(std::uint32_t slot);
		void FreeSampledImageSlotDeferred(std::uint32_t slot);
		void AdvanceFrame(std::uint64_t frameIndex);
		[[nodiscard]] Expected<void> UpdateSampledImage(std::uint32_t slot, VkImageView imageView, VkSampler sampler, VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	private:
		struct PendingSlotFree
		{
			std::uint32_t slot = 0;
			std::uint64_t releaseFrame = 0;
		};

		void FreeSlotImmediateUnlocked(std::uint32_t slot);

		mutable std::mutex m_mutex;
		VkDevice m_device = VK_NULL_HANDLE;
		VkDescriptorPool m_pool = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
		VkDescriptorSet m_set = VK_NULL_HANDLE;
		std::uint32_t m_capacity = 0;
		std::uint32_t m_deferredFreeFrames = 3;
		std::uint64_t m_currentFrame = 0;
		std::vector<std::uint32_t> m_freeSlots;
		std::vector<bool> m_slotAllocated;
		std::vector<PendingSlotFree> m_pendingSlotFrees;
		VkSampler m_linearSampler = VK_NULL_HANDLE;

		// Sampler deduplication cache: key -> VkSampler.
		std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> m_samplerCache;
	};
} // namespace aether
