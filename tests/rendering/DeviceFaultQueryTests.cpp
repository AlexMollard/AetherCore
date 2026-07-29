// Coverage for the VK_EXT_device_fault query - the code that runs after the GPU is already
// lost and is the only thing standing between a device loss and a bare crash with no
// information. It has to survive whatever the driver hands back, so it is driven here by a
// fake vkGetDeviceFaultInfoEXT that behaves badly on purpose: lying counts, unterminated
// description arrays, a retained vendor binary blob, VK_INCOMPLETE, outright failure.
//
// The regression that motivated this: the second call used to reuse the counts struct the
// first call populated, leaving vendorBinarySize non-zero while pVendorBinaryData was null.
// That is a declared capacity for a null buffer, and the NVIDIA driver wrote the blob to
// address 0 - an access violation inside the crash handler for the crash.
//
// No Vulkan device is needed: the entry point is a plain function pointer and the device
// handle is never dereferenced by the code under test.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vulkan/DeviceFaultQuery.hpp"

using namespace aether;

namespace
{
	struct FakeDriver
	{
		// What the first (counts) call reports.
		VkResult countResult = VK_SUCCESS;
		std::uint32_t availableAddressCount = 0;
		std::uint32_t availableVendorCount = 0;
		VkDeviceSize availableVendorBinarySize = 0;

		// What the second (records) call does.
		VkResult infoResult = VK_SUCCESS;
		std::vector<VkDeviceFaultAddressInfoEXT> addressPayload;
		std::vector<VkDeviceFaultVendorInfoEXT> vendorPayload;
		std::string description;
		bool unterminatedDescription = false;
		std::optional<std::uint32_t> writeBackAddressCount;
		std::optional<std::uint32_t> writeBackVendorCount;
		VkDeviceSize writeBackVendorBinarySize = 0;

		// What the caller actually asked for.
		int callCount = 0;
		bool sawRecordsCall = false;
		VkDeviceFaultCountsEXT recordsCallCounts{};
		const void* recordsCallVendorBinaryData = reinterpret_cast<const void*>(std::uintptr_t{1});
		// Set when the caller declared a non-zero vendor binary capacity for a null
		// pointer. A real driver dereferences that and takes the process down.
		bool blobWriteWouldFault = false;
	};

	FakeDriver g_driver;

	VKAPI_ATTR VkResult VKAPI_CALL FakeGetDeviceFaultInfo(VkDevice /*device*/, VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info)
	{
		++g_driver.callCount;

		if (info == nullptr)
		{
			counts->addressInfoCount = g_driver.availableAddressCount;
			counts->vendorInfoCount = g_driver.availableVendorCount;
			counts->vendorBinarySize = g_driver.availableVendorBinarySize;
			return g_driver.countResult;
		}

		g_driver.sawRecordsCall = true;
		g_driver.recordsCallCounts = *counts;
		g_driver.recordsCallVendorBinaryData = info->pVendorBinaryData;
		if (counts->vendorBinarySize != 0 && info->pVendorBinaryData == nullptr)
		{
			g_driver.blobWriteWouldFault = true;
		}

		if (g_driver.unterminatedDescription)
		{
			std::memset(info->description, 'A', sizeof(info->description));
		}
		else if (!g_driver.description.empty())
		{
			const std::size_t copied = std::min(g_driver.description.size(), sizeof(info->description) - 1);
			std::memcpy(info->description, g_driver.description.data(), copied);
			info->description[copied] = '\0';
		}

		const std::uint32_t addressWritten = std::min<std::uint32_t>(counts->addressInfoCount, static_cast<std::uint32_t>(g_driver.addressPayload.size()));
		for (std::uint32_t i = 0; i < addressWritten; ++i)
		{
			info->pAddressInfos[i] = g_driver.addressPayload[i];
		}

		const std::uint32_t vendorWritten = std::min<std::uint32_t>(counts->vendorInfoCount, static_cast<std::uint32_t>(g_driver.vendorPayload.size()));
		for (std::uint32_t i = 0; i < vendorWritten; ++i)
		{
			info->pVendorInfos[i] = g_driver.vendorPayload[i];
		}

		counts->addressInfoCount = g_driver.writeBackAddressCount.value_or(addressWritten);
		counts->vendorInfoCount = g_driver.writeBackVendorCount.value_or(vendorWritten);
		counts->vendorBinarySize = g_driver.writeBackVendorBinarySize;
		return g_driver.infoResult;
	}

	void ResetDriver()
	{
		g_driver = FakeDriver{};
	}

	VkDevice FakeDevice()
	{
		return reinterpret_cast<VkDevice>(std::uintptr_t{0xD1CE});
	}

	VkDeviceFaultAddressInfoEXT MakeAddress(VkDeviceFaultAddressTypeEXT type, VkDeviceAddress address)
	{
		VkDeviceFaultAddressInfoEXT out{};
		out.addressType = type;
		out.reportedAddress = address;
		out.addressPrecision = 4096;
		return out;
	}

	VkDeviceFaultVendorInfoEXT MakeVendor(std::uint64_t code, std::uint64_t data, const char* text, bool terminate)
	{
		VkDeviceFaultVendorInfoEXT out{};
		out.vendorFaultCode = code;
		out.vendorFaultData = data;
		if (terminate)
		{
			const std::size_t copied = std::min(std::strlen(text), sizeof(out.description) - 1);
			std::memcpy(out.description, text, copied);
		}
		else
		{
			std::memset(out.description, 'V', sizeof(out.description));
		}
		return out;
	}

	// A fault-info struct whose trailing members are poisoned with non-zero bytes, so a read
	// that runs off the end of the fixed description array hits them instead of stopping on
	// a zero byte by luck.
	VkDeviceFaultInfoEXT MakePoisonedInfo(bool unterminatedDescription)
	{
		VkDeviceFaultInfoEXT info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
		if (unterminatedDescription)
		{
			std::memset(info.description, 'A', sizeof(info.description));
		}
		constexpr std::uintptr_t kPoison = static_cast<std::uintptr_t>(0x0101010101010101ULL);
		info.pAddressInfos = reinterpret_cast<VkDeviceFaultAddressInfoEXT*>(kPoison);
		info.pVendorInfos = reinterpret_cast<VkDeviceFaultVendorInfoEXT*>(kPoison);
		info.pVendorBinaryData = reinterpret_cast<void*>(kPoison);
		return info;
	}

	VkDeviceFaultCountsEXT MakeCounts(std::uint32_t addressCount, std::uint32_t vendorCount, VkDeviceSize binarySize)
	{
		VkDeviceFaultCountsEXT counts{};
		counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
		counts.addressInfoCount = addressCount;
		counts.vendorInfoCount = vendorCount;
		counts.vendorBinarySize = binarySize;
		return counts;
	}

	bool AnyNoteContains(const vulkan::DeviceFaultReport& report, std::string_view needle)
	{
		for (const std::string& note: report.notes)
		{
			if (note.find(needle) != std::string::npos)
			{
				return true;
			}
		}
		return false;
	}
} // namespace

TEST_CASE("QueryDeviceFault never declares a vendor binary capacity for a null pointer")
{
	ResetDriver();
	g_driver.availableAddressCount = 1;
	g_driver.availableVendorBinarySize = 4096;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x1234'5678'9ABCULL));

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(g_driver.sawRecordsCall);
	CHECK(g_driver.recordsCallVendorBinaryData == nullptr);
	CHECK(g_driver.recordsCallCounts.vendorBinarySize == 0);
	CHECK_FALSE(g_driver.blobWriteWouldFault);

	// Declining the blob must not cost the rest of the record.
	REQUIRE(result.report.has_value());
	CHECK(result.report->addressInfos.size() == 1);
	CHECK(result.report->hasMemoryFault);
	CHECK(result.report->faultAddressInfo.reportedAddress == 0x1234'5678'9ABCULL);
	// ...and the blob the driver kept is still called out.
	CHECK(AnyNoteContains(*result.report, "4096 bytes of vendor binary"));
}

TEST_CASE("QueryDeviceFault passes the allocated capacity, not the driver's own numbers")
{
	ResetDriver();
	g_driver.availableAddressCount = 3;
	g_driver.availableVendorCount = 2;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x10));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x20));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x30));
	g_driver.vendorPayload.push_back(MakeVendor(1, 2, "a", true));
	g_driver.vendorPayload.push_back(MakeVendor(3, 4, "b", true));

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	CHECK(g_driver.callCount == 2);
	CHECK(g_driver.recordsCallCounts.addressInfoCount == 3);
	CHECK(g_driver.recordsCallCounts.vendorInfoCount == 2);
	REQUIRE(result.report.has_value());
	CHECK(result.report->addressInfos.size() == 3);
	CHECK(result.report->vendorInfos.size() == 2);
}

TEST_CASE("A driver that grows its counts on the second call cannot push the read past the buffer")
{
	ResetDriver();
	g_driver.availableAddressCount = 2;
	g_driver.availableVendorCount = 1;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0xAA));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0xBB));
	g_driver.vendorPayload.push_back(MakeVendor(7, 8, "vendor", true));
	g_driver.writeBackAddressCount = 99;
	g_driver.writeBackVendorCount = 50;

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->addressInfos.size() == 2);
	CHECK(result.report->vendorInfos.size() == 1);
	CHECK(AnyNoteContains(*result.report, "99 address records"));
	CHECK(AnyNoteContains(*result.report, "50 vendor records"));
}

TEST_CASE("A driver that shrinks its counts on the second call reports only what it wrote")
{
	ResetDriver();
	g_driver.availableAddressCount = 4;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT, 0xC0DE));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT, 0xC0DF));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT, 0xC0E0));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT, 0xC0E1));
	g_driver.writeBackAddressCount = 1;

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->addressInfos.size() == 1);
	CHECK(result.report->faultAddressInfo.reportedAddress == 0xC0DE);
	// The three slots the driver left untouched must not be reported as fault records.
	CHECK_FALSE(AnyNoteContains(*result.report, "not reported"));
}

TEST_CASE("An unterminated driver description is bounded to the array")
{
	ResetDriver();
	g_driver.availableAddressCount = 1;
	g_driver.unterminatedDescription = true;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x99));

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->description.size() == 256);
	CHECK(result.report->description == std::string(256, 'A'));
}

TEST_CASE("BuildDeviceFaultReport bounds an unterminated description without reading past it")
{
	const VkDeviceFaultInfoEXT info = MakePoisonedInfo(true);
	const VkDeviceFaultCountsEXT counts = MakeCounts(0, 0, 0);

	const vulkan::DeviceFaultReport report = vulkan::BuildDeviceFaultReport(info, {}, {}, counts, false);

	CHECK(report.description.size() == 256);
	CHECK(report.description == std::string(256, 'A'));
}

TEST_CASE("BuildDeviceFaultReport bounds an unterminated vendor description")
{
	const VkDeviceFaultInfoEXT info = MakePoisonedInfo(false);
	const std::vector<VkDeviceFaultVendorInfoEXT> vendors{MakeVendor(0xFEED, 0xFACE, "", false)};
	const VkDeviceFaultCountsEXT counts = MakeCounts(0, 1, 0);

	const vulkan::DeviceFaultReport report = vulkan::BuildDeviceFaultReport(info, {}, vendors, counts, false);

	REQUIRE(report.vendorInfos.size() == 1);
	CHECK(report.vendorInfos.front().description.size() == 256);
	CHECK(report.vendorInfos.front().description == std::string(256, 'V'));
	// With no info description of its own, the report falls back to the vendor text.
	CHECK(report.description.size() == 256);
}

TEST_CASE("BuildDeviceFaultReport clamps claimed counts to the buffers it was given")
{
	const VkDeviceFaultInfoEXT info = MakePoisonedInfo(false);
	const std::vector<VkDeviceFaultAddressInfoEXT> addresses{
	        MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x111),
	        MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x222),
	};
	const std::vector<VkDeviceFaultVendorInfoEXT> vendors{MakeVendor(5, 6, "one", true)};
	const VkDeviceFaultCountsEXT counts = MakeCounts(4000, 7, 0);

	const vulkan::DeviceFaultReport report = vulkan::BuildDeviceFaultReport(info, addresses, vendors, counts, false);

	CHECK(report.addressInfos.size() == 2);
	CHECK(report.vendorInfos.size() == 1);
	CHECK(report.faultAddressInfo.reportedAddress == 0x111);
	CHECK(AnyNoteContains(report, "4000 address records"));
	CHECK(AnyNoteContains(report, "7 vendor records"));
}

TEST_CASE("A vendor record with no address records still produces a report")
{
	ResetDriver();
	g_driver.availableVendorCount = 1;
	g_driver.vendorPayload.push_back(MakeVendor(0xDEADBEEF, 0x1234, "engine hang", true));

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->addressInfos.empty());
	CHECK(result.report->hasVendorInfo);
	CHECK_FALSE(result.report->hasMemoryFault);
	CHECK_FALSE(result.report->hasInstructionFault);
	CHECK(result.report->vendorFaultCode == 0xDEADBEEF);
	CHECK(result.report->vendorFaultData == 0x1234);
	CHECK(result.report->description == "engine hang");
}

TEST_CASE("An all-NONE address list is reported as unclassified rather than dropped")
{
	ResetDriver();
	g_driver.availableAddressCount = 3;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT, 0));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT, 0));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT, 0));

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->addressInfos.size() == 3);
	CHECK_FALSE(result.report->hasMemoryFault);
	CHECK_FALSE(result.report->hasInstructionFault);
	CHECK(AnyNoteContains(*result.report, "classified every one as 'not reported'"));
}

TEST_CASE("An instruction pointer record is classified separately from a memory fault")
{
	ResetDriver();
	g_driver.availableAddressCount = 2;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT, 0x7000));
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x8000));

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->hasInstructionFault);
	CHECK(result.report->hasMemoryFault);
	CHECK(result.report->instructionAddressInfo.reportedAddress == 0x7000);
	CHECK(result.report->faultAddressInfo.reportedAddress == 0x8000);
}

TEST_CASE("VK_INCOMPLETE from the records call keeps the data and says it was truncated")
{
	ResetDriver();
	g_driver.availableAddressCount = 1;
	g_driver.infoResult = VK_INCOMPLETE;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x4242));

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->faultAddressInfo.reportedAddress == 0x4242);
	CHECK(AnyNoteContains(*result.report, "VK_INCOMPLETE"));
}

TEST_CASE("A failing counts call reports the reason instead of returning silently")
{
	ResetDriver();
	g_driver.countResult = VK_ERROR_OUT_OF_HOST_MEMORY;

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	CHECK_FALSE(result.report.has_value());
	CHECK(result.status.find("counts") != std::string::npos);
	CHECK(g_driver.callCount == 1);
}

TEST_CASE("A failing records call reports the reason instead of returning silently")
{
	ResetDriver();
	g_driver.availableAddressCount = 1;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x1));
	g_driver.infoResult = VK_ERROR_DEVICE_LOST;

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	CHECK_FALSE(result.report.has_value());
	CHECK(result.status.find("records") != std::string::npos);
}

TEST_CASE("A driver that preserved nothing says so")
{
	ResetDriver();

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	CHECK_FALSE(result.report.has_value());
	CHECK(result.status.find("preserved no") != std::string::npos);
	// The second call still happens: a driver may supply only a description.
	CHECK(g_driver.callCount == 2);
}

TEST_CASE("A driver that preserved only a description still produces a report")
{
	ResetDriver();
	g_driver.description = "GPU hung in compute";

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(result.report->description == "GPU hung in compute");
	CHECK(result.report->addressInfos.empty());
}

TEST_CASE("QueryDeviceFault refuses to call through a missing entry point or a dead device")
{
	ResetDriver();

	const vulkan::DeviceFaultQueryResult missingEntryPoint = vulkan::QueryDeviceFault(nullptr, FakeDevice());
	CHECK_FALSE(missingEntryPoint.report.has_value());
	CHECK(missingEntryPoint.status.find("unavailable") != std::string::npos);

	const vulkan::DeviceFaultQueryResult deadDevice = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, VK_NULL_HANDLE);
	CHECK_FALSE(deadDevice.report.has_value());
	CHECK(deadDevice.status.find("No Vulkan device") != std::string::npos);
	CHECK(g_driver.callCount == 0);
}

TEST_CASE("A driver claiming it wrote a vendor blob that was never requested is called out")
{
	ResetDriver();
	g_driver.availableAddressCount = 1;
	g_driver.addressPayload.push_back(MakeAddress(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x5));
	g_driver.writeBackVendorBinarySize = 512;

	const vulkan::DeviceFaultQueryResult result = vulkan::QueryDeviceFault(&FakeGetDeviceFaultInfo, FakeDevice());

	REQUIRE(result.report.has_value());
	CHECK(AnyNoteContains(*result.report, "512 bytes of vendor binary data even though none was requested"));
}

TEST_CASE("Every address type has a distinct human-readable name")
{
	CHECK(vulkan::DeviceFaultAddressTypeText(VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT) == "Not reported");
	CHECK(vulkan::DeviceFaultAddressTypeText(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT) == "Read invalid");
	CHECK(vulkan::DeviceFaultAddressTypeText(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT) == "Write invalid");
	CHECK(vulkan::DeviceFaultAddressTypeText(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT) == "Execute invalid");
	CHECK(vulkan::DeviceFaultAddressTypeText(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT) == "Instruction pointer unknown");
	CHECK(vulkan::DeviceFaultAddressTypeText(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT) == "Instruction pointer invalid");
	CHECK(vulkan::DeviceFaultAddressTypeText(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT) == "Instruction pointer fault");
	// A driver inventing a value must not fall off the switch.
	CHECK(vulkan::DeviceFaultAddressTypeText(static_cast<VkDeviceFaultAddressTypeEXT>(4242)) == "Unknown(4242)");
}
