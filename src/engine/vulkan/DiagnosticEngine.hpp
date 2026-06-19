#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vulkan/volk.hpp"
#include "vulkan/GpuMemoryTracker.hpp"

namespace aether
{
	// Unified GPU Diagnostic Engine.
	//
	// Combines three post-mortem data sources into a single forensic dashboard:
	//
	//   1. VK_KHR_device_fault - raw hardware fault registers (memory address,
	//      instruction pointer, vendor code). Captured
	//      immediately after VK_ERROR_DEVICE_LOST.
	//
	//   2. VK_EXT_device_address_binding_report + GpuMemoryTracker - a live
	//      registry of every GPU virtual address range and its C++ resource
	//      name. Used to resolve raw fault addresses back to human-readable
	//      names like "Player_Vertex_Buffer".
	//
	//   3. GPU-side flight recorder - a host-visible ring buffer of breadcrumb
	//      markers written via vkCmdWriteBufferMarker2AMD before each
	//      draw/dispatch. After a crash, the last written marker identifies
	//      the exact command that faulted. Requires VK_AMD_buffer_marker.
	//
	// Usage:
	//   diagnosticEngine.Init(device, physicalDevice);
	//   // ... at allocation sites: diagnosticEngine.GetMemoryTracker().Register(...)
	//   // ... before each dispatch: diagnosticEngine.WriteBreadcrumb(cmd, label);
	//   // On VK_ERROR_DEVICE_LOST:
	//   diagnosticEngine.CaptureFaults();
	class DiagnosticEngine
	{
	public:
		// Initialize with an already-created VkDevice. Loads function pointers
		// for VK_KHR_device_fault and (if available) VK_AMD_buffer_marker.
		// If VK_AMD_buffer_marker is supported, allocates the flight-recorder
		// breadcrumb buffer (host-visible, persistently mapped).
		void Init(VkDevice device, VkPhysicalDevice physicalDevice);

		// Tear down all resources. Safe to call after device loss.
		void Shutdown();

		// Capture and log the full forensic dashboard. Queries
		// vkGetDeviceFaultReportsKHR, resolves every fault address via the
		// GpuMemoryTracker, dumps the breadcrumb ring buffer, and prints a
		// verbose structured report. Called automatically by VulkanContext
		// on VK_ERROR_DEVICE_LOST; can also be called manually.
		void CaptureFaults();

		// -- Memory tracking -------------------------------------------------

		[[nodiscard]] GpuMemoryTracker& GetMemoryTracker()
		{
			return m_memoryTracker;
		}

		// Register a GPU resource by its device address. Called at buffer /
		// heap allocation sites.
		void RegisterResource(VkDeviceAddress addr, VkDeviceSize size, std::string_view name, GpuMemoryTracker::ResourceType type)
		{
			m_memoryTracker.Register(addr, size, std::string(name), type);
		}

		void UnregisterResource(VkDeviceAddress addr)
		{
			m_memoryTracker.Unregister(addr);
		}

		// -- Flight recorder -------------------------------------------------

		// Write a breadcrumb marker into the GPU-side ring buffer. Call this
		// before every draw/dispatch in a command buffer. The marker value
		// encodes the frame index and a caller-provided label hash so the
		// last completed marker before a crash can be decoded to a
		// human-readable "frame N, pass X" string.
		//
		// No-op if VK_AMD_buffer_marker is unavailable or the engine is not
		// initialized.
		void WriteBreadcrumb(VkCommandBuffer cmd, std::uint32_t markerValue);

		// Register a human-readable label for a marker value. The flight
		// recorder uses this to decode the last-written marker after a crash.
		void RegisterBreadcrumbLabel(std::uint32_t markerValue, std::string_view label);

		// Advance to the next frame. Resets the per-frame breadcrumb counter
		// so marker values stay unique within a frame.
		void BeginFrame(std::uint64_t frameIndex);

		// Dump the breadcrumb ring buffer contents (last N markers).
		void DumpBreadcrumbs();

	private:
		void ResolveAndLogAddress(VkDeviceAddress addr, const char* tag) const;
		void DumpFaultFlags(VkDeviceFaultFlagsKHR flags) const;

		VkDevice m_device = VK_NULL_HANDLE;
		VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
		bool m_initialized = false;

		GpuMemoryTracker m_memoryTracker;

		// -- VK_KHR_device_fault function pointer
		PFN_vkGetDeviceFaultReportsKHR m_vkGetDeviceFaultReportsKHR = nullptr;

		// -- Flight recorder (VK_AMD_buffer_marker)
		bool m_hasBufferMarker = false;
		PFN_vkCmdWriteBufferMarker2AMD m_vkCmdWriteBufferMarker2AMD = nullptr;
		VkBuffer m_breadcrumbBuffer = VK_NULL_HANDLE;
		VkDeviceMemory m_breadcrumbMemory = VK_NULL_HANDLE;
		void* m_breadcrumbMapped = nullptr;
		static constexpr std::uint32_t kBreadcrumbSlotCount = 4096;
		std::atomic<std::uint32_t> m_nextBreadcrumbSlot{0};
		std::uint64_t m_currentFrameIndex = 0;

		// CPU-side label registry for marker values.
		mutable std::mutex m_labelMutex;
		std::unordered_map<std::uint32_t, std::string> m_breadcrumbLabels;
	};
} // namespace aether
