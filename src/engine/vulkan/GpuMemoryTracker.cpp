#include "vulkan/GpuMemoryTracker.hpp"

#include <algorithm>
#include <cstdint>

#include "utils/Logger.hpp"

namespace aether
{
	void GpuMemoryTracker::Register(VkDeviceAddress addr, VkDeviceSize size, std::string name, ResourceType type)
	{
		if (addr == 0 || size == 0)
		{
			return;
		}
		const std::unique_lock lock(m_mutex);
		m_ranges[addr] = Resource{.startAddress = addr, .size = size, .name = std::move(name), .type = type};
	}

	void GpuMemoryTracker::Unregister(VkDeviceAddress addr)
	{
		const std::unique_lock lock(m_mutex);
		m_ranges.erase(addr);
	}

	void GpuMemoryTracker::UnregisterRange(VkDeviceAddress addr, VkDeviceSize size)
	{
		const std::unique_lock lock(m_mutex);
		const auto end = addr + size;
		for (auto it = m_ranges.begin(); it != m_ranges.end();)
		{
			if (it->first >= addr && it->first < end)
			{
				it = m_ranges.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void GpuMemoryTracker::Clear()
	{
		const std::unique_lock lock(m_mutex);
		m_ranges.clear();
	}

	std::optional<GpuMemoryTracker::ResolvedAddress> GpuMemoryTracker::Resolve(VkDeviceAddress addr) const
	{
		const std::shared_lock lock(m_mutex);
		auto it = m_ranges.upper_bound(addr);
		if (it == m_ranges.begin())
		{
			return std::nullopt;
		}
		--it;

		const Resource* best = nullptr;
		VkDeviceSize bestSize = UINT64_MAX;
		for (auto cur = it;; --cur)
		{
			const auto& r = cur->second;
			if (addr >= r.startAddress && addr < r.startAddress + r.size)
			{
				if (r.size < bestSize)
				{
					bestSize = r.size;
					best = &r;
				}
			}
			if (cur == m_ranges.begin())
			{
				break;
			}
		}

		if (best == nullptr)
		{
			return std::nullopt;
		}
		return ResolvedAddress{.resource = best, .offset = addr - best->startAddress};
	}

	void GpuMemoryTracker::Dump() const
	{
		const std::shared_lock lock(m_mutex);
		AE_INFO(LogCategory::Vulkan, "GpuMemoryTracker: {} tracked range(s).", m_ranges.size());
		for (const auto& [addr, r]: m_ranges)
		{
			const char* typeStr = r.type == ResourceType::Buffer ? "Buffer" : r.type == ResourceType::Image ? "Image" : r.type == ResourceType::GpuHeap ? "GpuHeap" : "BindlessHeap";
			AE_INFO(LogCategory::Vulkan, "  [0x{:016X}..0x{:016X}) size={} type={} name='{}'", addr, addr + r.size, r.size, typeStr, r.name);
		}
	}

	std::size_t GpuMemoryTracker::Size() const
	{
		const std::shared_lock lock(m_mutex);
		return m_ranges.size();
	}
} // namespace aether
