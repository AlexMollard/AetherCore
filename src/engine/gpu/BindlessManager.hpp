#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gpu/DescriptorSetLayout.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "material/BindlessContract.hpp"
#include "utils/Assert.hpp"
#include "vulkan/volk.hpp"

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
			gpu::Filter filter = gpu::Filter::Linear;
			gpu::SamplerMipmapMode mipmapMode = gpu::SamplerMipmapMode::Linear;
			gpu::SamplerAddressMode addressMode = gpu::SamplerAddressMode::Repeat;
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
		[[nodiscard]] gpu::DescriptorSetLayout GetLayout() const;
		[[nodiscard]] gpu::DescriptorSet GetSet() const;
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

		// Returns a cached sampler matching the given filter/mipmap/address mode.
		// Creates a new sampler if no matching one exists. Thread-safe.
		[[nodiscard]] Expected<gpu::Sampler> GetOrCreateSampler(gpu::Filter filter, gpu::SamplerMipmapMode mipmapMode, gpu::SamplerAddressMode addressMode);

		[[nodiscard]] Expected<std::uint32_t> AllocateSampledImageSlot();
		void FreeSampledImageSlot(std::uint32_t slot);
		void FreeSampledImageSlotDeferred(std::uint32_t slot);
		void AdvanceFrame(std::uint64_t frameIndex);
		[[nodiscard]] Expected<void> UpdateSampledImage(std::uint32_t slot, gpu::ImageView imageView, gpu::Sampler sampler, gpu::ImageLayout imageLayout = gpu::ImageLayout::ShaderReadOnly);

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
