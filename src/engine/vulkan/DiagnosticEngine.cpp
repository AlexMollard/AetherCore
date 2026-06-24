#include "vulkan/DiagnosticEngine.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <string>

#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		std::string FormatHex(std::uint64_t value)
		{
			return std::format("0x{:016X}", value);
		}

		std::string FormatU32(std::uint32_t value)
		{
			return std::format("0x{:08X}", value);
		}

		std::string ResourceTypeToString(GpuMemoryTracker::ResourceType type)
		{
			switch (type)
			{
				case GpuMemoryTracker::ResourceType::Buffer:
					return "Buffer";
				case GpuMemoryTracker::ResourceType::Image:
					return "Image";
				case GpuMemoryTracker::ResourceType::GpuHeap:
					return "GpuHeap";
				case GpuMemoryTracker::ResourceType::BindlessHeap:
					return "BindlessHeap";
			}

			return "Unknown";
		}

		std::string DecodeFaultFlags(VkDeviceFaultFlagsKHR flags)
		{
			std::string out;
			if ((flags & VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR) != 0)
			{
				out += "Device Lost, ";
			}
			if ((flags & VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR) != 0)
			{
				out += "Invalid Memory Access, ";
			}
			if ((flags & VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR) != 0)
			{
				out += "Instruction Address, ";
			}
			if ((flags & VK_DEVICE_FAULT_FLAG_VENDOR_KHR) != 0)
			{
				out += "Vendor Diagnostic, ";
			}
			if ((flags & VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR) != 0)
			{
				out += "TDR Timeout, ";
			}
			if ((flags & VK_DEVICE_FAULT_FLAG_OVERFLOW_KHR) != 0)
			{
				out += "Overflow, ";
			}
			if (!out.empty())
			{
				out.resize(out.size() - 2);
			}
			return out.empty() ? "None" : out;
		}

		std::string OffsetText(VkDeviceSize offset, VkDeviceSize size)
		{
			if (size == 0)
			{
				return std::format("{} bytes (0%)", offset);
			}

			const double percent = (static_cast<double>(offset) * 100.0) / static_cast<double>(size);
			return std::format("{} bytes ({:.2f}%)", offset, percent);
		}

		std::string AddressTypeText(VkDeviceFaultAddressTypeKHR type)
		{
			switch (type)
			{
				case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR:
					return "Not reported";
				case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_KHR:
					return "Read invalid";
				case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_KHR:
					return "Write invalid";
				case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_KHR:
					return "Execute invalid";
				case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_KHR:
					return "Instruction pointer unknown";
				case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_KHR:
					return "Instruction pointer invalid";
				case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_KHR:
					return "Instruction pointer fault";
				case VK_DEVICE_FAULT_ADDRESS_TYPE_MAX_ENUM_KHR:
					return "Unknown";
				default:
					return std::format("Unknown({})", static_cast<int>(type));
			}
		}

		bool ParseUInt64Suffix(std::string_view text, std::string_view prefix, std::uint64_t& value)
		{
			if (!text.starts_with(prefix))
			{
				return false;
			}

			const std::string_view number = text.substr(prefix.size());
			std::uint64_t parsed = 0;
			const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), parsed);
			if (ec != std::errc{} || ptr != number.data() + number.size())
			{
				return false;
			}

			value = parsed;
			return true;
		}
	} // namespace

	void DiagnosticEngine::Init(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue)
	{
		if (m_initialized)
		{
			Shutdown();
		}

		m_device = device;
		m_physicalDevice = physicalDevice;
		m_graphicsQueue = graphicsQueue;

		VkPhysicalDeviceProperties props{};
		if (m_physicalDevice != VK_NULL_HANDLE)
		{
			vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
			m_vendorId = props.vendorID;
			m_vendorName = props.deviceName;
		}
		else
		{
			m_vendorId = 0;
			m_vendorName = "Unknown";
		}

		m_vkGetDeviceFaultReportsKHR = reinterpret_cast<PFN_vkGetDeviceFaultReportsKHR>(vkGetDeviceProcAddr(m_device, "vkGetDeviceFaultReportsKHR"));

		if (m_physicalDevice != VK_NULL_HANDLE)
		{
			uint32_t extCount = 0;
			if (vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr) != VK_SUCCESS)
			{
				return;
			}
			std::vector<VkExtensionProperties> exts(extCount);
			VkResult extResult = vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, exts.data());
			if (extResult != VK_SUCCESS && extResult != VK_INCOMPLETE)
			{
				return;
			}
			extCount = std::min<uint32_t>(extCount, static_cast<uint32_t>(exts.size()));

			for (std::uint32_t i = 0; i < extCount; ++i)
			{
				const auto& e = exts[i];
				if (std::strcmp(e.extensionName, VK_AMD_BUFFER_MARKER_EXTENSION_NAME) == 0)
				{
					TryInitAmdBufferMarker();
				}
				else if (std::strcmp(e.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) == 0)
				{
					TryInitNvCheckpoints();
				}
			}
		}

		m_initialized = true;
		AE_INFO(LogCategory::Vulkan, "DiagnosticEngine initialized (device_fault={}, amd_marker={}, nv_checkpoint={}).", m_vkGetDeviceFaultReportsKHR != nullptr ? "yes" : "no", m_hasBufferMarker ? "yes" : "no", m_hasCheckpointNV ? "yes" : "no");
	}

	void DiagnosticEngine::TryInitAmdBufferMarker()
	{
		m_vkCmdWriteBufferMarker2AMD = reinterpret_cast<PFN_vkCmdWriteBufferMarker2AMD>(vkGetDeviceProcAddr(m_device, "vkCmdWriteBufferMarker2AMD"));
		if (m_vkCmdWriteBufferMarker2AMD == nullptr)
		{
			return;
		}

		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = kBreadcrumbSlotCount * sizeof(uint32_t),
		        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VkBuffer breadcrumbBuf = VK_NULL_HANDLE;
		if (vkCreateBuffer(m_device, &bufInfo, nullptr, &breadcrumbBuf) != VK_SUCCESS)
		{
			return;
		}

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

		if (memTypeIndex == UINT32_MAX)
		{
			vkDestroyBuffer(m_device, breadcrumbBuf, nullptr);
			return;
		}

		const VkMemoryAllocateInfo allocInfo{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		        .allocationSize = memReqs.size,
		        .memoryTypeIndex = memTypeIndex,
		};
		VkDeviceMemory breadcrumbMem = VK_NULL_HANDLE;
		if (vkAllocateMemory(m_device, &allocInfo, nullptr, &breadcrumbMem) != VK_SUCCESS)
		{
			vkDestroyBuffer(m_device, breadcrumbBuf, nullptr);
			return;
		}

		if (vkBindBufferMemory(m_device, breadcrumbBuf, breadcrumbMem, 0) != VK_SUCCESS)
		{
			vkFreeMemory(m_device, breadcrumbMem, nullptr);
			vkDestroyBuffer(m_device, breadcrumbBuf, nullptr);
			return;
		}

		void* mapped = nullptr;
		if (vkMapMemory(m_device, breadcrumbMem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		{
			vkFreeMemory(m_device, breadcrumbMem, nullptr);
			vkDestroyBuffer(m_device, breadcrumbBuf, nullptr);
			return;
		}

		m_breadcrumbBuffer = breadcrumbBuf;
		m_breadcrumbMemory = breadcrumbMem;
		m_breadcrumbMapped = mapped;
		m_hasBufferMarker = true;
		std::memset(mapped, 0, kBreadcrumbSlotCount * sizeof(uint32_t));
		AE_INFO(LogCategory::Vulkan, "DiagnosticEngine: flight recorder enabled (VK_AMD_buffer_marker, {} slots).", kBreadcrumbSlotCount);
	}

	void DiagnosticEngine::TryInitNvCheckpoints()
	{
		m_vkCmdSetCheckpointNV = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(vkGetDeviceProcAddr(m_device, "vkCmdSetCheckpointNV"));
		m_vkGetQueueCheckpointDataNV = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(vkGetDeviceProcAddr(m_device, "vkGetQueueCheckpointDataNV"));
		if (m_vkCmdSetCheckpointNV != nullptr && m_vkGetQueueCheckpointDataNV != nullptr)
		{
			m_hasCheckpointNV = true;
			AE_INFO(LogCategory::Vulkan, "DiagnosticEngine: flight recorder enabled (VK_NV_device_diagnostic_checkpoints).");
		}
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
		{
			std::lock_guard lock(m_labelMutex);
			m_breadcrumbLabels.clear();
		}
		for (EventSlot& slot: m_eventSlots)
		{
			slot.sequence.store(std::uint64_t(-1), std::memory_order_relaxed);
		}
		m_nextBreadcrumbSlot.store(0, std::memory_order_relaxed);
		m_nextEventSlot.store(0, std::memory_order_relaxed);
		m_currentFrameIndex = 0;

		m_hasBufferMarker = false;
		m_hasCheckpointNV = false;
		m_vkGetDeviceFaultReportsKHR = nullptr;
		m_vkCmdWriteBufferMarker2AMD = nullptr;
		m_vkCmdSetCheckpointNV = nullptr;
		m_vkGetQueueCheckpointDataNV = nullptr;

		m_vendorId = 0;
		m_vendorName.clear();
		m_queueLabel = "Graphics";
		m_initialized = false;
		m_device = VK_NULL_HANDLE;
		m_physicalDevice = VK_NULL_HANDLE;
		m_graphicsQueue = VK_NULL_HANDLE;
	}

	void DiagnosticEngine::CaptureFaults()
	{
		bool expected = false;
		if (!m_captureInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed))
		{
			return;
		}

		if (!m_initialized || m_vkGetDeviceFaultReportsKHR == nullptr)
		{
			AE_WARN(LogCategory::Vulkan, "DiagnosticEngine::CaptureFaults: not available (device_fault extension missing or engine not initialized).");
			m_captureInProgress.store(false, std::memory_order_release);
			return;
		}

		const auto breadcrumbs = CollectBreadcrumbs();
		const auto faults = QueryFaultReports();
		const FaultAnalysis analysis = AnalyzeFaults(faults, breadcrumbs);
		const ActivitySummary activity = BuildActivitySummary();

		AE_DIAG_COLOR(LogPlainColor::BoldCyan, "============================================================");
		AE_DIAG_COLOR(LogPlainColor::BoldCyan, "GPU DEVICE LOSS DIAGNOSTIC REPORT");
		AE_DIAG_COLOR(LogPlainColor::BoldCyan, "============================================================");
		AE_DIAG("Frame: {} | GPU: {} ({}) | Queue: {}", m_currentFrameIndex, m_vendorName.empty() ? "Unknown" : m_vendorName.c_str(), m_vendorId == 0 ? "unknown" : FormatHex(m_vendorId).c_str(), m_queueLabel);

		AE_DIAG("");
		AE_DIAG_COLOR(LogPlainColor::BoldRed, "VERDICT");
		AE_DIAG_COLOR(LogPlainColor::BoldRed, "[WARN] {}", analysis.likelyCause.empty() ? "Unknown GPU device loss" : analysis.likelyCause.c_str());
		AE_DIAG("Confidence: {} - {}", analysis.confidence.empty() ? "LOW" : analysis.confidence.c_str(), analysis.confidenceReason.empty() ? "Limited diagnostic data." : analysis.confidenceReason.c_str());
		AE_DIAG("Location: {}", analysis.location.empty() ? "Unknown GPU work" : analysis.location.c_str());
		AE_DIAG("Resource: {}", analysis.resource.empty() ? "Unmapped / driver-internal address" : analysis.resource.c_str());

		AE_DIAG("");
		AE_DIAG_COLOR(LogPlainColor::BoldYellow, "LIKELY FAILURE POINT");
		if (breadcrumbs.empty())
		{
			AE_DIAG_COLOR(LogPlainColor::BoldRed, "[FAIL] No AMD/NV breadcrumb was recorded before device loss.");
			AE_DIAG("Suspected: unknown GPU work.");
		}
		else
		{
			const auto& last = breadcrumbs.front();
			AE_DIAG_COLOR(LogPlainColor::BoldGreen,
			        "[OK] Last completed: {} {}",
			        last.label.empty() ? "(unregistered)" : last.label.c_str(),
			        last.hasStage ? std::format("(stage=0x{:X})", static_cast<std::uint32_t>(last.value)).c_str() : std::format("(marker={})", FormatU32(last.value)).c_str());
			if (breadcrumbs.size() > 1)
			{
				const auto& next = breadcrumbs[1];
				AE_DIAG_COLOR(LogPlainColor::BoldYellow, "[WARN] Suspected: work submitted after '{}' and before '{}'.", last.label.empty() ? "(unregistered)" : last.label.c_str(), next.label.empty() ? "(unregistered)" : next.label.c_str());
			}
			else
			{
				AE_DIAG_COLOR(LogPlainColor::BoldYellow, "[WARN] Suspected: work submitted after '{}'; no later breadcrumb was recorded.", last.label.empty() ? "(unregistered)" : last.label.c_str());
			}
		}

		AE_DIAG("");
		AE_DIAG_COLOR(LogPlainColor::BoldRed, "FAULT INFORMATION");
		if (faults.empty())
		{
			AE_DIAG_COLOR(LogPlainColor::BoldYellow, "[WARN] No hardware fault records preserved by driver.");
		}
		else
		{
			const auto& f = faults.front();
			AE_DIAG("Type: {}", DecodeFaultFlags(f.flags));
			if (f.description[0] != '\0')
			{
				AE_DIAG("Driver: {}", f.description);
			}
			if (f.faultAddressInfo.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR && f.faultAddressInfo.reportedAddress != 0)
			{
				AE_DIAG("Fault address: {}", FormatHex(f.faultAddressInfo.reportedAddress));
				AE_DIAG("Fault address type: {}", AddressTypeText(f.faultAddressInfo.addressType));
				const auto resolved = m_memoryTracker.Resolve(f.faultAddressInfo.reportedAddress);
				if (resolved.has_value())
				{
					const auto* r = resolved->resource;
					AE_DIAG_COLOR(LogPlainColor::BoldGreen, "Resolved resource: {} ({}), offset {}", r->name, ResourceTypeToString(r->type), OffsetText(resolved->offset, r->size));
				}
				else
				{
					AE_DIAG_COLOR(LogPlainColor::BoldYellow, "Resolved resource: unmapped / driver-internal address");
				}
			}
			else
			{
				AE_DIAG("Fault address: not reported");
			}
			if (f.instructionAddressInfo.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR && f.instructionAddressInfo.reportedAddress != 0)
			{
				AE_DIAG("Instruction pointer: {}", FormatHex(f.instructionAddressInfo.reportedAddress));
				AE_DIAG("Instruction pointer type: {}", AddressTypeText(f.instructionAddressInfo.addressType));
				AE_DIAG_COLOR(LogPlainColor::BoldMagenta, "Plain meaning: GPU was executing shader code at this address; it is not a buffer/image allocation.");
			}
			else
			{
				AE_DIAG("Instruction pointer: not reported");
			}
		}

		if (!faults.empty() && faults.front().vendorInfo.description[0] != '\0')
		{
			const auto& vendor = faults.front().vendorInfo;
			AE_DIAG("");
			AE_DIAG_COLOR(LogPlainColor::BoldMagenta, "VENDOR INFORMATION");
			AE_DIAG("Code: 0x{:X} | Data: 0x{:X} | {}", vendor.vendorFaultCode, vendor.vendorFaultData, vendor.description);
		}

		AE_DIAG("");
		AE_DIAG_COLOR(LogPlainColor::BoldCyan, "GPU WORKLOAD");
		if (activity.passes.empty())
		{
			AE_DIAG_COLOR(LogPlainColor::BoldYellow, "[WARN] No per-pass workload was recorded.");
		}
		else
		{
			for (const PassWorkload& pass: activity.passes)
			{
				if (pass.completed)
				{
					AE_DIAG_COLOR(LogPlainColor::BoldGreen, "[OK] {} - {}", pass.name.empty() ? "(unknown)" : pass.name.c_str(), WorkloadText(pass));
				}
				else
				{
					AE_DIAG_COLOR(LogPlainColor::BoldYellow, "[WARN] {} - {}", pass.name.empty() ? "(unknown)" : pass.name.c_str(), WorkloadText(pass));
				}
			}
		}

		if (activity.previousFrame.has_value() && activity.currentFrame.has_value())
		{
			AE_DIAG("");
			AE_DIAG_COLOR(LogPlainColor::BoldMagenta, "WHAT CHANGED");
			AE_DIAG("Frame {} vs {}:", *activity.previousFrame, *activity.currentFrame);
			for (const PassWorkload& pass: activity.passes)
			{
				const auto prevIt = std::find_if(activity.previousPasses.begin(), activity.previousPasses.end(), [&](const PassWorkload& p) { return p.name == pass.name; });
				const std::uint64_t prevTotal =
				        prevIt == activity.previousPasses.end() ? 0 : prevIt->directDraws + prevIt->indexedDraws + prevIt->indirectDraws + prevIt->indirectIndexedDraws + prevIt->indirectCountDraws + prevIt->dispatches + prevIt->fillBuffers;
				const std::uint64_t total = pass.directDraws + pass.indexedDraws + pass.indirectDraws + pass.indirectIndexedDraws + pass.indirectCountDraws + pass.dispatches + pass.fillBuffers;
				if (prevIt == activity.previousPasses.end() || prevTotal != total)
				{
					AE_DIAG_COLOR(LogPlainColor::Magenta, "{}: {} -> {}", pass.name.empty() ? "(unknown)" : pass.name.c_str(), prevIt == activity.previousPasses.end() ? "new" : std::to_string(prevTotal).c_str(), total);
				}
			}
		}

		AE_DIAG("");
		AE_DIAG_COLOR(LogPlainColor::BoldGreen, "RECOMMENDED INVESTIGATION");
		if (analysis.nextSteps.empty())
		{
			AE_DIAG_COLOR(LogPlainColor::BoldYellow, "[WARN] No specific next step was generated.");
		}
		else
		{
			for (const std::string& step: analysis.nextSteps)
			{
				AE_DIAG_COLOR(LogPlainColor::Yellow, "[WARN] {}", step);
			}
		}

		AE_DIAG_COLOR(LogPlainColor::BoldCyan, "============================================================");
		m_captureInProgress.store(false, std::memory_order_release);
	}

	void DiagnosticEngine::WriteBreadcrumb(VkCommandBuffer cmd, std::uint32_t markerValue)
	{
		if (m_hasBufferMarker && m_vkCmdWriteBufferMarker2AMD != nullptr && m_breadcrumbBuffer != VK_NULL_HANDLE)
		{
			const VkDeviceSize offset = (static_cast<VkDeviceSize>(m_nextBreadcrumbSlot.fetch_add(1, std::memory_order_relaxed)) % kBreadcrumbSlotCount) * sizeof(uint32_t);
			m_vkCmdWriteBufferMarker2AMD(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_breadcrumbBuffer, offset, markerValue);
		}
		else if (m_hasCheckpointNV && m_vkCmdSetCheckpointNV != nullptr)
		{
			m_vkCmdSetCheckpointNV(cmd, reinterpret_cast<const void*>(static_cast<uintptr_t>(markerValue)));
			m_nextBreadcrumbSlot.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void DiagnosticEngine::RegisterBreadcrumbLabel(std::uint32_t markerValue, std::string_view label)
	{
		std::lock_guard lock(m_labelMutex);
		m_breadcrumbLabels[markerValue] = std::string(label);
	}

	void DiagnosticEngine::BeginFrame(std::uint64_t frameIndex)
	{
		m_currentFrameIndex = frameIndex;
		RecordEvent("Begin Frame {}", frameIndex);
	}

	void DiagnosticEngine::EndFrame(const std::uint64_t frameIndex)
	{
		RecordEvent("End Frame {}", frameIndex);
	}

	void DiagnosticEngine::RecordEvent(std::string_view message)
	{
		constexpr std::uint64_t kWriting = std::uint64_t(-2);

		const std::uint64_t sequence = m_nextEventSlot.fetch_add(1, std::memory_order_relaxed);
		EventSlot& slot = m_eventSlots[sequence % kEventRingCount];
		std::uint64_t expected = sequence >= kEventRingCount ? sequence - kEventRingCount : std::uint64_t(-1);
		if (!slot.sequence.compare_exchange_strong(expected, kWriting, std::memory_order_acq_rel, std::memory_order_relaxed))
		{
			return;
		}

		const std::size_t copyCount = std::min(message.size(), kEventMessageSize - 1);
		std::copy_n(message.data(), copyCount, slot.message.data());
		slot.message[copyCount] = '\0';
		slot.sequence.store(sequence, std::memory_order_release);
	}

	DiagnosticEngine::PassWorkload* DiagnosticEngine::FindOrAddPass(std::vector<PassWorkload>& passes, const std::string_view name)
	{
		for (PassWorkload& pass: passes)
		{
			if (pass.name == name)
			{
				return &pass;
			}
		}
		passes.push_back(PassWorkload{.name = std::string(name)});
		return &passes.back();
	}

	std::string DiagnosticEngine::WorkloadText(const PassWorkload& pass)
	{
		std::string out =
		        std::format("{} direct, {} indexed, {} indirect, {} indexed-indirect, {} indirect-count, {} dispatches", pass.directDraws, pass.indexedDraws, pass.indirectDraws, pass.indirectIndexedDraws, pass.indirectCountDraws, pass.dispatches);
		if (pass.fillBuffers != 0)
		{
			out += std::format(", {} fill-buffer", pass.fillBuffers);
		}
		if (!pass.completed)
		{
			out += " [IN PROGRESS]";
		}
		return out;
	}

	DiagnosticEngine::ActivitySummary DiagnosticEngine::BuildActivitySummary() const
	{
		DiagnosticEngine::ActivitySummary summary;
		std::vector<DiagnosticEngine::PassWorkload> currentPasses;
		std::optional<std::uint64_t> activeFrame;
		PassWorkload* activePass = nullptr;
		std::map<std::uint64_t, std::vector<DiagnosticEngine::PassWorkload>> frames;

		const std::uint64_t nextEvent = m_nextEventSlot.load(std::memory_order_relaxed);
		const std::uint64_t eventCount = std::min<std::uint64_t>(nextEvent, kEventRingCount);
		const std::uint64_t start = nextEvent - eventCount;

		for (std::uint64_t i = start; i < nextEvent; ++i)
		{
			const EventSlot& slot = m_eventSlots[i % kEventRingCount];
			if (slot.sequence.load(std::memory_order_acquire) != i)
			{
				continue;
			}

			const std::string_view eventView{slot.message.data()};
			std::uint64_t parsedFrame = 0;
			if (ParseUInt64Suffix(eventView, "Begin Frame ", parsedFrame))
			{
				summary.previousFrame = summary.currentFrame;
				summary.currentFrame = parsedFrame;
				activeFrame = parsedFrame;
				currentPasses.clear();
				activePass = nullptr;
				continue;
			}

			if (ParseUInt64Suffix(eventView, "End Frame ", parsedFrame))
			{
				if (activeFrame.has_value())
				{
					frames[*activeFrame] = currentPasses;
				}
				currentPasses.clear();
				activePass = nullptr;
				continue;
			}

			if (eventView.starts_with("Begin pass "))
			{
				const std::string_view name = eventView.substr(std::char_traits<char>::length("Begin pass "));
				activePass = FindOrAddPass(currentPasses, name);
				continue;
			}

			if (eventView.starts_with("End pass "))
			{
				const std::string_view name = eventView.substr(std::char_traits<char>::length("End pass "));
				if (activePass != nullptr && activePass->name == name)
				{
					activePass->completed = true;
				}
				else
				{
					for (DiagnosticEngine::PassWorkload& pass: currentPasses)
					{
						if (pass.name == name)
						{
							pass.completed = true;
							break;
						}
					}
				}
				activePass = nullptr;
				continue;
			}

			if (activePass == nullptr)
			{
				continue;
			}

			if (eventView.starts_with("Draw("))
			{
				++activePass->directDraws;
			}
			else if (eventView.starts_with("DrawIndexed("))
			{
				++activePass->indexedDraws;
			}
			else if (eventView.starts_with("DrawIndirect("))
			{
				++activePass->indirectDraws;
			}
			else if (eventView.starts_with("DrawIndexedIndirectCount("))
			{
				++activePass->indirectCountDraws;
			}
			else if (eventView.starts_with("DrawIndexedIndirect("))
			{
				++activePass->indirectIndexedDraws;
			}
			else if (eventView.starts_with("Dispatch("))
			{
				++activePass->dispatches;
			}
			else if (eventView.starts_with("FillBuffer("))
			{
				++activePass->fillBuffers;
			}
		}

		summary.passes = std::move(currentPasses);
		if (summary.currentFrame.has_value() && frames.contains(*summary.currentFrame))
		{
			summary.passes = frames[*summary.currentFrame];
		}
		if (summary.previousFrame.has_value() && frames.contains(*summary.previousFrame))
		{
			summary.previousPasses = frames[*summary.previousFrame];
		}
		else
		{
			summary.previousFrame.reset();
		}
		return summary;
	}

	std::vector<DiagnosticEngine::ResolvedBreadcrumb> DiagnosticEngine::CollectBreadcrumbs()
	{
		std::vector<ResolvedBreadcrumb> out;
		if (m_hasCheckpointNV && m_graphicsQueue != VK_NULL_HANDLE && m_vkGetQueueCheckpointDataNV != nullptr)
		{
			uint32_t checkpointCount = 0;
			m_vkGetQueueCheckpointDataNV(m_graphicsQueue, &checkpointCount, nullptr);
			if (checkpointCount != 0)
			{
				std::vector<VkCheckpointDataNV> checkpoints(checkpointCount);
				for (uint32_t i = 0; i < checkpointCount; ++i)
				{
					checkpoints[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
					checkpoints[i].pNext = nullptr;
				}

				m_vkGetQueueCheckpointDataNV(m_graphicsQueue, &checkpointCount, checkpoints.data());
				const uint32_t dumpCount = std::min(checkpointCount, uint32_t{16});
				for (uint32_t i = 0; i < dumpCount; ++i)
				{
					const auto& cp = checkpoints[i];
					const std::uint32_t val = static_cast<std::uint32_t>(reinterpret_cast<uintptr_t>(cp.pCheckpointMarker));
					out.push_back(ResolvedBreadcrumb{
					        .value = val,
					        .label = GetLabel(val),
					        .stage = std::format("0x{:X}", static_cast<uint32_t>(cp.stage)),
					        .hasStage = true,
					});
				}
			}
		}
		else if (m_hasBufferMarker && m_breadcrumbMapped != nullptr)
		{
			const auto* slots = static_cast<const std::uint32_t*>(m_breadcrumbMapped);
			const std::uint64_t nextSlot = m_nextBreadcrumbSlot.load(std::memory_order_relaxed);
			const std::uint32_t slotsToScan = static_cast<std::uint32_t>(std::min<std::uint64_t>(nextSlot, kBreadcrumbSlotCount));
			const std::uint32_t startSlot = static_cast<std::uint32_t>(nextSlot % kBreadcrumbSlotCount);
			const std::uint32_t contextCount = std::min(slotsToScan, std::uint32_t{16});
			for (std::uint32_t i = 0; i < contextCount; ++i)
			{
				const std::uint32_t slotIdx = (startSlot + kBreadcrumbSlotCount - 1 - i) % kBreadcrumbSlotCount;
				const std::uint32_t val = slots[slotIdx];
				if (val == 0)
				{
					continue;
				}
				out.push_back(ResolvedBreadcrumb{
				        .value = val,
				        .label = GetLabel(val),
				        .stage = "unknown",
				        .hasStage = false,
				});
			}
		}

		return out;
	}

	std::vector<VkDeviceFaultInfoKHR> DiagnosticEngine::QueryFaultReports()
	{
		std::vector<VkDeviceFaultInfoKHR> faults;
		if (m_vkGetDeviceFaultReportsKHR == nullptr)
		{
			return faults;
		}

		uint32_t faultCount = 0;
		VkResult result = m_vkGetDeviceFaultReportsKHR(m_device, 0, &faultCount, nullptr);
		if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || faultCount == 0)
		{
			return faults;
		}

		faults.resize(faultCount);
		for (uint32_t i = 0; i < faultCount; ++i)
		{
			faults[i].sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR;
			faults[i].pNext = nullptr;
		}

		result = m_vkGetDeviceFaultReportsKHR(m_device, 0, &faultCount, faults.data());
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		{
			faults.clear();
		}

		return faults;
	}

	std::string DiagnosticEngine::GetLabel(std::uint32_t markerValue) const
	{
		std::lock_guard lock(m_labelMutex);
		auto it = m_breadcrumbLabels.find(markerValue);
		if (it == m_breadcrumbLabels.end())
		{
			return {};
		}
		return it->second;
	}

	DiagnosticEngine::FaultAnalysis DiagnosticEngine::AnalyzeFaults(const std::vector<VkDeviceFaultInfoKHR>& faults, const std::vector<ResolvedBreadcrumb>& breadcrumbs) const
	{
		FaultAnalysis analysis;
		if (faults.empty())
		{
			analysis.likelyCause = "Device lost without preserved hardware fault records";
			analysis.confidence = "MEDIUM";
			analysis.confidenceReason = "The device was lost, but vkGetDeviceFaultReportsKHR did not preserve a detailed fault record.";
			return analysis;
		}

		const auto& f = faults.front();
		analysis.location = breadcrumbs.empty() ? "Unknown GPU work" : breadcrumbs.front().label;
		if (analysis.location.empty())
		{
			analysis.location = "Unregistered breadcrumb";
		}

		if (f.vendorInfo.description[0] != '\0')
		{
			analysis.likelyCause = f.vendorInfo.description;
			analysis.confidence = "HIGH";
			analysis.confidenceReason = "Driver supplied an explicit vendor fault description.";
		}
		else if ((f.flags & VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR) != 0)
		{
			analysis.likelyCause = "GPU hang / TDR timeout";
			analysis.confidence = "HIGH";
			analysis.confidenceReason = "The driver reported a watchdog timeout, which is a direct hang signal.";
		}
		else if ((f.flags & VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR) != 0)
		{
			const auto resolved = m_memoryTracker.Resolve(f.faultAddressInfo.reportedAddress);
			if (resolved.has_value())
			{
				analysis.resource = resolved->resource->name;
				analysis.likelyCause = std::format("GPU memory access fault inside tracked resource '{}'", analysis.resource);
				analysis.confidence = "HIGH";
				analysis.confidenceReason = std::format("Fault address resolves to '{}', so the invalid access can be tied to a live resource range.", analysis.resource);
			}
			else
			{
				analysis.likelyCause = "GPU memory access fault at an unmapped or stale GPU address";
				analysis.confidence = "MEDIUM-HIGH";
				analysis.confidenceReason = "The driver reported an invalid GPU memory address, but it did not match any tracked allocation.";
			}
		}
		else if ((f.flags & VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR) != 0)
		{
			analysis.likelyCause = "Shader instruction pointer fault";
			analysis.confidence = "MEDIUM";
			analysis.confidenceReason = "The instruction pointer points inside shader code, not a CPU resource allocation, so the pass breadcrumb is more reliable than address resolution.";
		}
		else if ((f.flags & VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR) != 0)
		{
			analysis.likelyCause = "Device lost without detailed fault classification";
			analysis.confidence = "MEDIUM";
			analysis.confidenceReason = "The device was lost, but the preserved fault record did not classify the root cause.";
		}
		else
		{
			analysis.likelyCause = "Unknown GPU fault";
			analysis.confidence = "LOW";
			analysis.confidenceReason = "No strong fault flag, vendor description, or resolved resource was available.";
		}

		if ((f.flags & VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR) != 0)
		{
			const auto resolved = m_memoryTracker.Resolve(f.faultAddressInfo.reportedAddress);
			if (resolved.has_value())
			{
				analysis.possibleCauses.push_back(std::format("Shader read/write out of bounds inside '{}'", resolved->resource->name));
				analysis.possibleCauses.push_back("Bindless descriptor heap entry points to the wrong GPU address or byte range");
				analysis.possibleCauses.push_back("Descriptor heap update wrote a valid resource with an incorrect offset/size");
				analysis.possibleCauses.push_back("Resource layout or format does not match what the shader expects");
				analysis.nextSteps.push_back(std::format("Inspect shader BDA/bindless indexing in '{}'", analysis.location));
				analysis.nextSteps.push_back("Dump bindless descriptor heap entries and verify BDA ranges before the faulting pass");
				analysis.nextSteps.push_back("Check buffer/image bounds for this frame");
			}
			else
			{
				analysis.possibleCauses.push_back("Shader computed a stale or invalid BDA from bindless descriptor data");
				analysis.possibleCauses.push_back("Bindless descriptor heap entry points to a destroyed or stale allocation");
				analysis.possibleCauses.push_back("Descriptor heap update race, bad byte offset, or bad stride");
				analysis.possibleCauses.push_back("Driver-internal shader-code address fault");
				analysis.nextSteps.push_back(std::format("Verify bindless descriptor heap writes and resource lifetimes before '{}'", analysis.location));
				analysis.nextSteps.push_back("Inspect shader code for invalid BDA/address calculations");
				analysis.nextSteps.push_back("Add shader debug printf or sentinel-buffer asserts around the suspected access");
			}
		}
		else if ((f.flags & VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR) != 0)
		{
			analysis.possibleCauses.push_back("Shader object instruction fetch fault");
			analysis.possibleCauses.push_back("Invalid or corrupted shader object/code path");
			analysis.possibleCauses.push_back("Driver-internal shader-code address is not resolvable to a CPU resource");
			analysis.nextSteps.push_back("Rebuild shaders with debug info and inspect the faulting shader object");
			analysis.nextSteps.push_back("Use debug printf or sentinel-buffer asserts in the faulting shader");
		}

		analysis.nextSteps.push_back("Enable synchronization validation for this run");
		analysis.nextSteps.push_back("Re-run with GPU-assisted validation if available");
		return analysis;
	}
} // namespace aether
