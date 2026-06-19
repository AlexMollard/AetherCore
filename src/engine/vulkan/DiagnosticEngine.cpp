#include "vulkan/DiagnosticEngine.hpp"

#include <algorithm>
#include <cstring>
#include <cstdint>

#include "utils/Logger.hpp"

namespace aether
{
	void DiagnosticEngine::Init(VkDevice device, VkPhysicalDevice physicalDevice)
	{
		m_device = device;
		m_physicalDevice = physicalDevice;

		m_vkGetDeviceFaultReportsKHR = reinterpret_cast<PFN_vkGetDeviceFaultReportsKHR>(vkGetDeviceProcAddr(m_device, "vkGetDeviceFaultReportsKHR"));

		// Check for VK_AMD_buffer_marker support and load the function pointer.
		// The extension is device-level; query the device extension list.
		bool hasAmdBufferMarker = false;
		if (m_physicalDevice != VK_NULL_HANDLE)
		{
			uint32_t extCount = 0;
			vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
			std::vector<VkExtensionProperties> exts(extCount);
			vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, exts.data());
			for (const auto& e: exts)
			{
				if (std::strcmp(e.extensionName, VK_AMD_BUFFER_MARKER_EXTENSION_NAME) == 0)
				{
					hasAmdBufferMarker = true;
					break;
				}
			}
		}

		if (hasAmdBufferMarker)
		{
			m_vkCmdWriteBufferMarker2AMD = reinterpret_cast<PFN_vkCmdWriteBufferMarker2AMD>(vkGetDeviceProcAddr(m_device, "vkCmdWriteBufferMarker2AMD"));
			if (m_vkCmdWriteBufferMarker2AMD != nullptr)
			{
				// Allocate a host-visible, persistently mapped buffer for the
				// breadcrumb ring buffer. This survives device loss because
				// the mapping is CPU-side - the GPU writes markers into it
				// during execution and we read the last written value after
				// the crash.
				const VkBufferCreateInfo bufInfo{
				        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				        .size = kBreadcrumbSlotCount * sizeof(uint32_t),
				        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				};
				VkBuffer breadcrumbBuf = VK_NULL_HANDLE;
				if (vkCreateBuffer(m_device, &bufInfo, nullptr, &breadcrumbBuf) == VK_SUCCESS)
				{
					VkMemoryRequirements memReqs{};
					vkGetBufferMemoryRequirements(m_device, breadcrumbBuf, &memReqs);

					VkPhysicalDeviceMemoryProperties memProps{};
					vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

					uint32_t memTypeIndex = UINT32_MAX;
					for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
					{
						if ((memReqs.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
						{
							memTypeIndex = i;
							break;
						}
					}

					if (memTypeIndex != UINT32_MAX)
					{
						const VkMemoryAllocateInfo allocInfo{
						        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
						        .allocationSize = memReqs.size,
						        .memoryTypeIndex = memTypeIndex,
						};
						VkDeviceMemory breadcrumbMem = VK_NULL_HANDLE;
						if (vkAllocateMemory(m_device, &allocInfo, nullptr, &breadcrumbMem) == VK_SUCCESS)
						{
							if (vkBindBufferMemory(m_device, breadcrumbBuf, breadcrumbMem, 0) == VK_SUCCESS)
							{
								void* mapped = nullptr;
								if (vkMapMemory(m_device, breadcrumbMem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
								{
									m_breadcrumbBuffer = breadcrumbBuf;
									m_breadcrumbMemory = breadcrumbMem;
									m_breadcrumbMapped = mapped;
									m_hasBufferMarker = true;
									std::memset(mapped, 0, kBreadcrumbSlotCount * sizeof(uint32_t));
									AE_INFO(LogCategory::Vulkan, "DiagnosticEngine: flight recorder enabled (VK_AMD_buffer_marker, {} slots).", kBreadcrumbSlotCount);
								}
								else
								{
									vkFreeMemory(m_device, breadcrumbMem, nullptr);
								}
							}
							else
							{
								vkFreeMemory(m_device, breadcrumbMem, nullptr);
							}
						}
					}
					if (!m_hasBufferMarker)
					{
						vkDestroyBuffer(m_device, breadcrumbBuf, nullptr);
					}
				}
			}
		}

		if (!m_hasBufferMarker)
		{
			AE_INFO(LogCategory::Vulkan, "DiagnosticEngine: VK_AMD_buffer_marker not available; flight recorder disabled (fault capture still active).");
		}

		m_initialized = true;
		AE_INFO(LogCategory::Vulkan, "DiagnosticEngine initialized (device_fault={}, breadcrumbs={}).", m_vkGetDeviceFaultReportsKHR != nullptr ? "yes" : "no", m_hasBufferMarker ? "yes" : "no");
	}

	void DiagnosticEngine::Shutdown()
	{
		if (!m_initialized)
		{
			return;
		}

		if (m_breadcrumbMapped != nullptr && m_device != VK_NULL_HANDLE)
		{
			vkUnmapMemory(m_device, m_breadcrumbMemory);
			m_breadcrumbMapped = nullptr;
		}
		if (m_breadcrumbBuffer != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(m_device, m_breadcrumbBuffer, nullptr);
			m_breadcrumbBuffer = VK_NULL_HANDLE;
		}
		if (m_breadcrumbMemory != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_device, m_breadcrumbMemory, nullptr);
			m_breadcrumbMemory = VK_NULL_HANDLE;
		}

		m_memoryTracker.Clear();
		m_hasBufferMarker = false;
		m_initialized = false;
		m_device = VK_NULL_HANDLE;
		m_physicalDevice = VK_NULL_HANDLE;
	}

	void DiagnosticEngine::CaptureFaults()
	{
		if (!m_initialized || m_vkGetDeviceFaultReportsKHR == nullptr)
		{
			AE_WARN(LogCategory::Vulkan, "DiagnosticEngine::CaptureFaults: not available (device_fault extension missing or engine not initialized).");
			return;
		}

		AE_ERROR(LogCategory::Vulkan, "");
		AE_ERROR(LogCategory::Vulkan, "================================================================");
		AE_ERROR(LogCategory::Vulkan, "              UNIFIED GPU DIAGNOSTIC ENGINE REPORT              ");
		AE_ERROR(LogCategory::Vulkan, "================================================================");

		// -- Phase 1: Flight recorder dump (before fault query - the mapped
		// memory may become invalid after fault query on some drivers).
		DumpBreadcrumbs();

		// -- Phase 2: VK_KHR_device_fault query.
		uint32_t faultCount = 0;
		VkResult result = m_vkGetDeviceFaultReportsKHR(m_device, 0, &faultCount, nullptr);
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		{
			AE_ERROR(LogCategory::Vulkan, "vkGetDeviceFaultReportsKHR (count) failed: VkResult={}.", static_cast<int>(result));
			AE_ERROR(LogCategory::Vulkan, "================================================================");
			return;
		}
		if (faultCount == 0)
		{
			AE_ERROR(LogCategory::Vulkan, "VK_KHR_device_fault: driver preserved no hardware diagnostic records.");
			AE_ERROR(LogCategory::Vulkan, "================================================================");
			return;
		}

		std::vector<VkDeviceFaultInfoKHR> faults(faultCount);
		for (uint32_t i = 0; i < faultCount; ++i)
		{
			faults[i].sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR;
		}

		result = m_vkGetDeviceFaultReportsKHR(m_device, 0, &faultCount, faults.data());
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		{
			AE_ERROR(LogCategory::Vulkan, "vkGetDeviceFaultReportsKHR (info) failed: VkResult={}.", static_cast<int>(result));
			AE_ERROR(LogCategory::Vulkan, "================================================================");
			return;
		}

		AE_ERROR(LogCategory::Vulkan, "");
		AE_ERROR(LogCategory::Vulkan, "-- VK_KHR_device_fault: {} fault record(s) --", faultCount);

		for (uint32_t i = 0; i < faultCount; ++i)
		{
			const auto& f = faults[i];

			AE_ERROR(LogCategory::Vulkan, "");
			AE_ERROR(LogCategory::Vulkan, "===== Fault[{}] groupId={} =====", i, f.groupId);
			DumpFaultFlags(f.flags);
			AE_ERROR(LogCategory::Vulkan, "  description: \"{}\"", f.description);

			// -- Memory (MMU) fault address resolution.
			if (f.faultAddressInfo.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR)
			{
				const auto& ai = f.faultAddressInfo;
				const char* typeStr = ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_KHR                  ? "ReadInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_KHR               ? "WriteInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_KHR             ? "ExecuteInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_KHR ? "InstrPtrUnknown"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_KHR ? "InstrPtrInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_KHR   ? "InstrPtrFault"
				                                                                                                       : "Unknown";
				AE_ERROR(LogCategory::Vulkan, "  faultAddress: type={} addr=0x{:016X} precision={}", typeStr, ai.reportedAddress, ai.addressPrecision);
				ResolveAndLogAddress(ai.reportedAddress, "faultAddress");
			}

			// -- Instruction pointer resolution.
			if (f.instructionAddressInfo.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR)
			{
				const auto& ii = f.instructionAddressInfo;
				AE_ERROR(LogCategory::Vulkan, "  instructionAddress: type={} addr=0x{:016X} precision={}", static_cast<int>(ii.addressType), ii.reportedAddress, ii.addressPrecision);
				ResolveAndLogAddress(ii.reportedAddress, "instructionAddress");
			}

			// -- Vendor-specific fault code.
			if (f.vendorInfo.description[0] != '\0')
			{
				const auto& vi = f.vendorInfo;
				AE_ERROR(LogCategory::Vulkan, "  vendor: faultCode=0x{:016X} faultData=0x{:016X} description=\"{}\"", vi.vendorFaultCode, vi.vendorFaultData, vi.description);
			}
		}

		AE_ERROR(LogCategory::Vulkan, "");
		AE_ERROR(LogCategory::Vulkan, "-- Tracked GPU resources ({} total) --", m_memoryTracker.Size());
		m_memoryTracker.Dump();

		AE_ERROR(LogCategory::Vulkan, "");
		AE_ERROR(LogCategory::Vulkan, "================================================================");
		AE_ERROR(LogCategory::Vulkan, "           END UNIFIED GPU DIAGNOSTIC ENGINE REPORT             ");
		AE_ERROR(LogCategory::Vulkan, "================================================================");
	}

	void DiagnosticEngine::WriteBreadcrumb(VkCommandBuffer cmd, std::uint32_t markerValue)
	{
		if (!m_hasBufferMarker || m_vkCmdWriteBufferMarker2AMD == nullptr || m_breadcrumbBuffer == VK_NULL_HANDLE)
		{
			return;
		}
		const VkDeviceSize offset = (static_cast<VkDeviceSize>(m_nextBreadcrumbSlot) % kBreadcrumbSlotCount) * sizeof(uint32_t);
		// BOTTOM_OF_PIPE: the marker is written after the preceding commands
		// complete. The last written marker in the ring buffer after a crash
		// is therefore the last successfully completed command.
		m_vkCmdWriteBufferMarker2AMD(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_breadcrumbBuffer, offset, markerValue);
		m_nextBreadcrumbSlot.fetch_add(1, std::memory_order_relaxed);
	}

	void DiagnosticEngine::RegisterBreadcrumbLabel(std::uint32_t markerValue, std::string_view label)
	{
		std::lock_guard lock(m_labelMutex);
		m_breadcrumbLabels[markerValue] = std::string(label);
	}

	void DiagnosticEngine::BeginFrame(std::uint64_t frameIndex)
	{
		m_currentFrameIndex = frameIndex;
	}

	void DiagnosticEngine::DumpBreadcrumbs()
	{
		if (!m_hasBufferMarker || m_breadcrumbMapped == nullptr)
		{
			AE_ERROR(LogCategory::Vulkan, "");
			AE_ERROR(LogCategory::Vulkan, "-- Flight recorder: disabled (VK_AMD_buffer_marker unavailable) --");
			return;
		}

		const auto* slots = static_cast<const std::uint32_t*>(m_breadcrumbMapped);
		const std::uint32_t slotsToScan = std::min(m_nextBreadcrumbSlot.load(), kBreadcrumbSlotCount);

		AE_ERROR(LogCategory::Vulkan, "");
		AE_ERROR(LogCategory::Vulkan, "-- Flight recorder: {} breadcrumb(s) written, scanning last {} --", m_nextBreadcrumbSlot.load(), slotsToScan);

		// Find the last non-zero marker. Because we use BOTTOM_OF_PIPE, the
		// last non-zero slot is the last command that completed before the
		// crash. The faulting command is the one immediately after.
		std::uint32_t lastNonZero = 0;
		std::uint32_t lastNonZeroSlot = 0;
		const std::uint32_t startSlot = (m_nextBreadcrumbSlot.load() % kBreadcrumbSlotCount);
		for (std::uint32_t i = 0; i < slotsToScan; ++i)
		{
			// Walk backwards from the most recent slot.
			const std::uint32_t slotIdx = (startSlot + kBreadcrumbSlotCount - 1 - i) % kBreadcrumbSlotCount;
			const std::uint32_t val = slots[slotIdx];
			if (val != 0)
			{
				lastNonZero = val;
				lastNonZeroSlot = slotIdx;
				break;
			}
		}

		if (lastNonZero == 0)
		{
			AE_ERROR(LogCategory::Vulkan, "  No completed breadcrumb markers found (GPU may have crashed before first command completed).");
			return;
		}

		std::string label;
		{
			std::lock_guard lock(m_labelMutex);
			auto it = m_breadcrumbLabels.find(lastNonZero);
			if (it != m_breadcrumbLabels.end())
			{
				label = it->second;
			}
		}

		AE_ERROR(LogCategory::Vulkan, "  Last completed GPU command: marker=0x{:08X} slot={} label=\"{}\"", lastNonZero, lastNonZeroSlot, label.empty() ? "(unregistered)" : label);
		AE_ERROR(LogCategory::Vulkan, "  -> The crash likely occurred in the command immediately AFTER this one.");

		// Also dump the last 16 markers for context.
		const std::uint32_t contextCount = std::min(slotsToScan, std::uint32_t{16});
		AE_ERROR(LogCategory::Vulkan, "  Recent breadcrumb history (last {}):", contextCount);
		for (std::uint32_t i = 0; i < contextCount; ++i)
		{
			const std::uint32_t slotIdx = (startSlot + kBreadcrumbSlotCount - 1 - i) % kBreadcrumbSlotCount;
			const std::uint32_t val = slots[slotIdx];
			if (val == 0)
			{
				continue;
			}
			std::string ctxLabel;
			{
				std::lock_guard lock(m_labelMutex);
				auto it = m_breadcrumbLabels.find(val);
				if (it != m_breadcrumbLabels.end())
				{
					ctxLabel = it->second;
				}
			}
			AE_ERROR(LogCategory::Vulkan, "    [{}] marker=0x{:08X} label=\"{}\"", slotIdx, val, ctxLabel.empty() ? "(unregistered)" : ctxLabel);
		}
	}

	void DiagnosticEngine::ResolveAndLogAddress(VkDeviceAddress addr, const char* tag) const
	{
		auto resolved = m_memoryTracker.Resolve(addr);
		if (resolved.has_value())
		{
			const auto* r = resolved->resource;
			const char* typeStr = r->type == GpuMemoryTracker::ResourceType::Buffer ? "Buffer" : r->type == GpuMemoryTracker::ResourceType::Image ? "Image" : r->type == GpuMemoryTracker::ResourceType::GpuHeap ? "GpuHeap" : "BindlessHeap";
			AE_ERROR(LogCategory::Vulkan, "    -> {} RESOLVED: name=\"{}\" type={} range=[0x{:016X}..0x{:016X}) offset=+{} bytes", tag, r->name, typeStr, r->startAddress, r->startAddress + r->size, resolved->offset);
		}
		else
		{
			AE_ERROR(LogCategory::Vulkan, "    -> {} UNRESOLVED: address 0x{:016X} does not match any tracked resource.", tag, addr);
		}
	}

	void DiagnosticEngine::DumpFaultFlags(VkDeviceFaultFlagsKHR flags) const
	{
		std::string flagStr;
		if (flags == 0)
		{
			flagStr = "(none)";
		}
		else
		{
			if (flags & VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR)
			{
				flagStr += "DeviceLost|";
			}
			if (flags & VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR)
			{
				flagStr += "MemoryAddress|";
			}
			if (flags & VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR)
			{
				flagStr += "InstructionAddress|";
			}
			if (flags & VK_DEVICE_FAULT_FLAG_VENDOR_KHR)
			{
				flagStr += "Vendor|";
			}
			if (flags & VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR)
			{
				flagStr += "WatchdogTimeout(TDR)|";
			}
			if (flags & VK_DEVICE_FAULT_FLAG_OVERFLOW_KHR)
			{
				flagStr += "Overflow|";
			}
			if (!flagStr.empty() && flagStr.back() == '|')
			{
				flagStr.pop_back();
			}
		}
		AE_ERROR(LogCategory::Vulkan, "  flags: 0x{:X} ({})", static_cast<uint32_t>(flags), flagStr);
	}
} // namespace aether
