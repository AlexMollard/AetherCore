#pragma once

// The engine's single implementation of the VK_EXT_device_fault query.
//
// This code only ever runs after the GPU has already been lost, so it is written to be
// incapable of making the situation worse: every value the driver hands back is treated
// as untrusted, the two-call idiom is spec-exact (in particular the vendor binary blob is
// explicitly declined by zeroing vendorBinarySize - leaving it set while passing a null
// pVendorBinaryData makes the NVIDIA driver write the blob to address 0), and nothing here
// touches engine state a device loss may already have torn down.
//
// The parsing half (BuildDeviceFaultReport) is a pure function so it can be exercised
// against hostile driver responses without a GPU - see tests/rendering/DeviceFaultQueryTests.cpp.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vulkan/volk.hpp"

namespace aether::vulkan
{
	// A driver vendor record with its description copied out of the fixed char array and
	// bounded, so an unterminated array can never be read past.
	struct DeviceFaultVendorInfo
	{
		std::string description;
		std::uint64_t vendorFaultCode = 0;
		std::uint64_t vendorFaultData = 0;
	};

	struct DeviceFaultReport
	{
		std::string description;
		std::vector<VkDeviceFaultAddressInfoEXT> addressInfos;
		std::vector<DeviceFaultVendorInfo> vendorInfos;

		// Anything unusual the driver did that a reader of the report should know about
		// (records dropped, data truncated, a vendor crash dump left behind, ...).
		std::vector<std::string> notes;

		bool hasMemoryFault = false;
		bool hasInstructionFault = false;
		bool hasVendorInfo = false;
		std::uint64_t vendorFaultCode = 0;
		std::uint64_t vendorFaultData = 0;
		VkDeviceFaultAddressInfoEXT faultAddressInfo{};
		VkDeviceFaultAddressInfoEXT instructionAddressInfo{};
	};

	// `report` is empty when the driver preserved nothing usable; `status` always says why
	// so the caller can print a reason instead of silently reporting nothing.
	struct DeviceFaultQueryResult
	{
		std::optional<DeviceFaultReport> report;
		std::string status;
	};

	[[nodiscard]] std::string DeviceFaultAddressTypeText(VkDeviceFaultAddressTypeEXT type);

	// Pure: turns one driver response into a report. `addressStorage`/`vendorStorage` are the
	// buffers the driver was given, `writtenCounts` is what it claimed to have written into
	// them (never trusted past the span sizes), and `truncated` reflects VK_INCOMPLETE.
	[[nodiscard]] DeviceFaultReport BuildDeviceFaultReport(
	        const VkDeviceFaultInfoEXT& info, std::span<const VkDeviceFaultAddressInfoEXT> addressStorage, std::span<const VkDeviceFaultVendorInfoEXT> vendorStorage, const VkDeviceFaultCountsEXT& writtenCounts, bool truncated);

	// Runs the two-call idiom against `queryFn`. Tolerates a null function pointer and a
	// null device so a caller running during teardown cannot trip a null call.
	[[nodiscard]] DeviceFaultQueryResult QueryDeviceFault(PFN_vkGetDeviceFaultInfoEXT queryFn, VkDevice device);

	// Dumps a result through the Vulkan log category. Used by callers that have no richer
	// report of their own to fold the fault into.
	void LogDeviceFaultReport(const DeviceFaultQueryResult& result);
} // namespace aether::vulkan
