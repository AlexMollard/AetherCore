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
	// human-readable C++ resource metadata. Populated at allocation time;
	// queried post-mortem by the DiagnosticEngine to resolve raw fault
	// addresses (from VK_KHR_device_fault) back to named resources like
	// "Player_Vertex_Buffer" or "GpuHeap.MeshArena".
	//
	// Lookup is O(log n) via a std::map keyed by start address. Sub-allocations
	// from a GpuHeap share the base buffer's BDA range and may overlap; the
	// resolver returns the innermost (most specific) match by preferring the
	// smallest containing range.
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

		// Remove the range starting exactly at addr. No-op if absent.
		void Unregister(VkDeviceAddress addr);

		// Remove all ranges whose start falls within [addr, addr+size).
		void UnregisterRange(VkDeviceAddress addr, VkDeviceSize size);

		void Clear();

		// Resolve a fault address to the containing resource. If multiple
		// registered ranges contain the address (e.g. a GpuHeap base buffer
		// and a sub-allocation), the smallest range wins. Returns nullopt if
		// no registered range contains addr.
		[[nodiscard]] std::optional<ResolvedAddress> Resolve(VkDeviceAddress addr) const;

		// Log every tracked range (for leak / coverage auditing).
		void Dump() const;

		[[nodiscard]] std::size_t Size() const;

	private:
		mutable std::shared_mutex m_mutex;
		std::map<VkDeviceAddress, Resource> m_ranges;
	};
} // namespace aether
