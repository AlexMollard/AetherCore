#include "vulkan/DeviceFaultQuery.hpp"

#include <algorithm>
#include <format>

#include "utils/Logger.hpp"

namespace aether::vulkan
{
	namespace
	{
		// The driver's description fields are fixed char arrays with no guarantee of a
		// terminator. Never hand one straight to std::string.
		std::string BoundedText(const char* text, std::size_t capacity)
		{
			if (text == nullptr || capacity == 0)
			{
				return {};
			}

			std::size_t length = 0;
			while (length < capacity && text[length] != '\0')
			{
				++length;
			}
			return std::string(text, length);
		}

		bool IsInstructionAddressType(VkDeviceFaultAddressTypeEXT type)
		{
			return type == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT || type == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT || type == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT;
		}

		// A device loss should never turn into a multi-gigabyte allocation because the
		// driver wrote a garbage count into the counts struct.
		constexpr std::uint32_t kMaxFaultRecords = 4096;
	} // namespace

	std::string DeviceFaultAddressTypeText(VkDeviceFaultAddressTypeEXT type)
	{
		switch (type)
		{
			case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
				return "Not reported";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
				return "Read invalid";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
				return "Write invalid";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
				return "Execute invalid";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
				return "Instruction pointer unknown";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
				return "Instruction pointer invalid";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
				return "Instruction pointer fault";
			// Enum sentinel (0x7FFFFFFF), never a real address type. Listed so the switch
			// stays exhaustive; a driver that invents a value lands in the default arm.
			case VK_DEVICE_FAULT_ADDRESS_TYPE_MAX_ENUM_KHR:
			default:
				return std::format("Unknown({})", static_cast<int>(type));
		}
	}

	DeviceFaultReport BuildDeviceFaultReport(
	        const VkDeviceFaultInfoEXT& info, std::span<const VkDeviceFaultAddressInfoEXT> addressStorage, std::span<const VkDeviceFaultVendorInfoEXT> vendorStorage, const VkDeviceFaultCountsEXT& writtenCounts, bool truncated)
	{
		DeviceFaultReport report;
		report.description = BoundedText(static_cast<const char*>(info.description), VK_MAX_DESCRIPTION_SIZE);

		// The driver rewrites the counts struct on the second call. Per spec it reports
		// what it actually wrote, which cannot exceed the capacity it was given - but a
		// count larger than the buffer would be an out-of-bounds read, so clamp it and
		// say so rather than trusting the number.
		const std::size_t claimedAddressCount = writtenCounts.addressInfoCount;
		const std::size_t addressCount = std::min(claimedAddressCount, addressStorage.size());
		if (claimedAddressCount > addressStorage.size())
		{
			report.notes.push_back(std::format("Driver reported {} address records but was only given room for {}; the surplus was ignored.", claimedAddressCount, addressStorage.size()));
		}

		const std::size_t claimedVendorCount = writtenCounts.vendorInfoCount;
		const std::size_t vendorCount = std::min(claimedVendorCount, vendorStorage.size());
		if (claimedVendorCount > vendorStorage.size())
		{
			report.notes.push_back(std::format("Driver reported {} vendor records but was only given room for {}; the surplus was ignored.", claimedVendorCount, vendorStorage.size()));
		}

		if (truncated)
		{
			report.notes.emplace_back("vkGetDeviceFaultInfoEXT returned VK_INCOMPLETE: the driver had more fault data than was retrieved.");
		}

		if (writtenCounts.vendorBinarySize != 0)
		{
			// We always decline the blob, so the driver claiming it wrote one means it
			// ignored the requested size. Worth recording; the data itself is discarded.
			report.notes.push_back(std::format("Driver reported writing {} bytes of vendor binary data even though none was requested.", writtenCounts.vendorBinarySize));
		}

		report.addressInfos.reserve(addressCount);
		std::size_t reportedAddressTypes = 0;
		for (std::size_t i = 0; i < addressCount; ++i)
		{
			const VkDeviceFaultAddressInfoEXT& addressInfo = addressStorage[i];
			report.addressInfos.push_back(addressInfo);
			if (addressInfo.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT)
			{
				continue;
			}

			++reportedAddressTypes;
			if (IsInstructionAddressType(addressInfo.addressType))
			{
				report.hasInstructionFault = true;
				if (report.instructionAddressInfo.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT)
				{
					report.instructionAddressInfo = addressInfo;
				}
			}
			else
			{
				report.hasMemoryFault = true;
				if (report.faultAddressInfo.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT)
				{
					report.faultAddressInfo = addressInfo;
				}
			}
		}

		if (addressCount != 0 && reportedAddressTypes == 0)
		{
			report.notes.push_back(std::format("Driver supplied {} address records but classified every one as 'not reported'; no fault address is available.", addressCount));
		}

		report.vendorInfos.reserve(vendorCount);
		for (std::size_t i = 0; i < vendorCount; ++i)
		{
			const VkDeviceFaultVendorInfoEXT& vendorInfo = vendorStorage[i];
			report.vendorInfos.push_back(DeviceFaultVendorInfo{
			        .description = BoundedText(static_cast<const char*>(vendorInfo.description), VK_MAX_DESCRIPTION_SIZE),
			        .vendorFaultCode = vendorInfo.vendorFaultCode,
			        .vendorFaultData = vendorInfo.vendorFaultData,
			});
		}

		if (!report.vendorInfos.empty())
		{
			report.hasVendorInfo = true;
			report.vendorFaultCode = report.vendorInfos.front().vendorFaultCode;
			report.vendorFaultData = report.vendorInfos.front().vendorFaultData;
			if (report.description.empty())
			{
				report.description = report.vendorInfos.front().description;
			}
		}

		return report;
	}

	DeviceFaultQueryResult QueryDeviceFault(PFN_vkGetDeviceFaultInfoEXT queryFn, VkDevice device)
	{
		DeviceFaultQueryResult result;
		if (queryFn == nullptr)
		{
			result.status = "VK_EXT_device_fault is unavailable (vkGetDeviceFaultInfoEXT was never loaded).";
			return result;
		}
		if (device == VK_NULL_HANDLE)
		{
			result.status = "No Vulkan device is live, so no fault record can be queried.";
			return result;
		}

		VkDeviceFaultCountsEXT available{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,
		};
		const VkResult countResult = queryFn(device, &available, nullptr);
		if (countResult != VK_SUCCESS && countResult != VK_INCOMPLETE)
		{
			result.status = std::format("vkGetDeviceFaultInfoEXT (counts) failed: VkResult={}.", static_cast<int>(countResult));
			return result;
		}

		const VkDeviceSize retainedVendorBinarySize = available.vendorBinarySize;
		const std::uint32_t addressCapacity = std::min(available.addressInfoCount, kMaxFaultRecords);
		const std::uint32_t vendorCapacity = std::min(available.vendorInfoCount, kMaxFaultRecords);

		// One slack element beyond what the driver is told it may write, so pAddressInfos /
		// pVendorInfos are always a valid writable allocation - including when the counts
		// came back zero, where an empty vector's data() would be null.
		std::vector<VkDeviceFaultAddressInfoEXT> addressStorage(static_cast<std::size_t>(addressCapacity) + 1);
		std::vector<VkDeviceFaultVendorInfoEXT> vendorStorage(static_cast<std::size_t>(vendorCapacity) + 1);

		VkDeviceFaultInfoEXT info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
		info.pAddressInfos = addressStorage.data();
		info.pVendorInfos = vendorStorage.data();
		info.pVendorBinaryData = nullptr;

		// The counts struct passed to the second call declares the capacity of the buffers
		// in `info`. vendorBinarySize MUST be zero here: it is the declared capacity of
		// pVendorBinaryData, and a driver told "you may write N bytes" to a null pointer
		// will do exactly that.
		VkDeviceFaultCountsEXT request{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,
		        .addressInfoCount = addressCapacity,
		        .vendorInfoCount = vendorCapacity,
		        .vendorBinarySize = 0,
		};

		const VkResult infoResult = queryFn(device, &request, &info);
		if (infoResult != VK_SUCCESS && infoResult != VK_INCOMPLETE)
		{
			result.status = std::format("vkGetDeviceFaultInfoEXT (records) failed: VkResult={}.", static_cast<int>(infoResult));
			return result;
		}

		DeviceFaultReport report = BuildDeviceFaultReport(
		        info, std::span<const VkDeviceFaultAddressInfoEXT>(addressStorage.data(), addressCapacity), std::span<const VkDeviceFaultVendorInfoEXT>(vendorStorage.data(), vendorCapacity), request, infoResult == VK_INCOMPLETE);

		if (available.addressInfoCount > addressCapacity || available.vendorInfoCount > vendorCapacity)
		{
			report.notes.push_back(std::format("Driver offered {} address and {} vendor records; only {}/{} were retrieved (per-query cap).", available.addressInfoCount, available.vendorInfoCount, addressCapacity, vendorCapacity));
		}
		if (retainedVendorBinarySize != 0)
		{
			report.notes.push_back(std::format("Driver retained {} bytes of vendor binary crash data; it is not requested here (fetching it is a vendor-tool step).", retainedVendorBinarySize));
		}

		if (report.description.empty() && report.addressInfos.empty() && report.vendorInfos.empty())
		{
			result.status = retainedVendorBinarySize != 0 ? std::format("Driver preserved no address, vendor or description records (it did retain {} bytes of vendor binary crash data).", retainedVendorBinarySize)
			                                              : "Driver preserved no address, vendor or description records for this device loss.";
			return result;
		}

		result.status = std::format("Retrieved {} address record(s) and {} vendor record(s).", report.addressInfos.size(), report.vendorInfos.size());
		result.report = std::move(report);
		return result;
	}

	void LogDeviceFaultReport(const DeviceFaultQueryResult& result)
	{
		AE_ERROR(LogCategory::Vulkan, "=================== VK_EXT_device_fault report ===================");
		if (!result.report.has_value())
		{
			AE_ERROR(LogCategory::Vulkan, "  {}", result.status);
			AE_ERROR(LogCategory::Vulkan, "=================== end device fault report ===================");
			return;
		}

		const DeviceFaultReport& report = *result.report;
		AE_ERROR(LogCategory::Vulkan, "  description: {}", report.description.empty() ? "(none supplied)" : report.description);

		for (std::size_t i = 0; i < report.addressInfos.size(); ++i)
		{
			const VkDeviceFaultAddressInfoEXT& addressInfo = report.addressInfos[i];
			AE_ERROR(LogCategory::Vulkan, "  addressInfo[{}]: type={} reportedAddress=0x{:016X} precision={}", i, DeviceFaultAddressTypeText(addressInfo.addressType), addressInfo.reportedAddress, addressInfo.addressPrecision);
		}

		for (std::size_t i = 0; i < report.vendorInfos.size(); ++i)
		{
			const DeviceFaultVendorInfo& vendorInfo = report.vendorInfos[i];
			AE_ERROR(LogCategory::Vulkan, "  vendorInfo[{}]: faultCode=0x{:016X} faultData=0x{:016X} description='{}'", i, vendorInfo.vendorFaultCode, vendorInfo.vendorFaultData, vendorInfo.description);
		}

		for (const std::string& note: report.notes)
		{
			AE_ERROR(LogCategory::Vulkan, "  note: {}", note);
		}

		AE_ERROR(LogCategory::Vulkan, "=================== end device fault report ===================");
	}
} // namespace aether::vulkan
