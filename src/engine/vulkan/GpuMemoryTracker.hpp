#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "vulkan/volk.hpp"

namespace aether
{
	// Thread-safe registry mapping GPU virtual address ranges (BDA) to
	class GpuMemoryTracker
	{
	public:
		enum class ResourceType : std::uint8_t
		{
			Buffer,
			Image,
			GpuHeap,
			BindlessHeap,
		};

		struct Resource
		{
			VkDeviceAddress startAddress = 0;
			VkDeviceSize size = 0;
			std::string name;
			ResourceType type = ResourceType::Buffer;
		};

		struct ResolvedAddress
		{
			const Resource* resource = nullptr;
			VkDeviceSize offset = 0;
		};

		void Register(VkDeviceAddress addr, VkDeviceSize size, std::string name, ResourceType type);

		void Unregister(VkDeviceAddress addr);

		void UnregisterRange(VkDeviceAddress addr, VkDeviceSize size);

		void Clear();

		[[nodiscard]] std::optional<ResolvedAddress> Resolve(VkDeviceAddress addr) const;

		void Dump() const;

		[[nodiscard]] std::size_t Size() const;

	private:
		mutable std::shared_mutex m_mutex;
		std::map<VkDeviceAddress, Resource> m_ranges;
	};
} // namespace aether
