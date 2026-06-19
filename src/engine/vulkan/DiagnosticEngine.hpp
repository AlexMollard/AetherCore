#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
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
	//   3. GPU-side flight recorder - breadcrumb markers written before each
	//      draw/dispatch using either VK_AMD_buffer_marker (buffer write) or
	//      VK_NV_device_diagnostic_checkpoints (driver-stored). After a crash
	//      the last completed marker identifies the exact command that faulted.
	//
	// Usage:
	//   diagnosticEngine.Init(device, physicalDevice, graphicsQueue);
	//   // ... at allocation sites: diagnosticEngine.GetMemoryTracker().Register(...)
	//   // ... before each dispatch: diagnosticEngine.WriteBreadcrumb(cmd, label);
	//   // On VK_ERROR_DEVICE_LOST:
	//   diagnosticEngine.CaptureFaults();
	class DiagnosticEngine
	{
	public:
		DiagnosticEngine() = default;
		DiagnosticEngine(const DiagnosticEngine&) = delete;
		DiagnosticEngine& operator=(const DiagnosticEngine&) = delete;

		// Initialize with an already-created VkDevice. Loads function pointers
		// for VK_KHR_device_fault, VK_AMD_buffer_marker, and
		// VK_NV_device_diagnostic_checkpoints (whichever the device supports).
		//
		// If VK_AMD_buffer_marker is supported, allocates the flight-recorder
		// breadcrumb buffer (host-visible, persistently mapped). Requires the
		// graphics queue for the NV checkpoint query fallback.
		void Init(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue);

		// Tear down all resources. Safe to call after device loss.
		void Shutdown();

		// Capture and log the full forensic dashboard. Queries
		// vkGetDeviceFaultReportsKHR, resolves every fault address via the
		// GpuMemoryTracker, dumps the breadcrumb ring buffer, and prints a
		// structured human-readable report. Called automatically by VulkanContext
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
		// encodes a caller-provided label hash so the last completed marker
		// before a crash can be decoded to a human-readable label.
		//
		// Uses whichever flight-recorder mechanism is available:
		//   - VK_AMD_buffer_marker (ring buffer write, all vendors)
		//   - VK_NV_device_diagnostic_checkpoints (driver-stored, Nvidia)
		// No-op if neither is available.
		void WriteBreadcrumb(VkCommandBuffer cmd, std::uint32_t markerValue);

		// Register a human-readable label for a marker value. The flight
		// recorder uses this to decode the last-written marker after a crash.
		void RegisterBreadcrumbLabel(std::uint32_t markerValue, std::string_view label);

		// Advance to the next frame. Resets the per-frame breadcrumb counter
		// so marker values stay unique within a frame.
		void BeginFrame(std::uint64_t frameIndex);
		void EndFrame(std::uint64_t frameIndex);

		// Lightweight CPU-side event ring used to reconstruct the recent
		// command sequence around a crash.
		void RecordEvent(std::string_view message);

		template<typename... Args>
		void RecordEvent(std::string_view formatString, Args&&... args)
		{
			RecordEvent(std::vformat(formatString, std::make_format_args(args...)));
		}

	private:
		struct ResolvedBreadcrumb
		{
			std::uint32_t value = 0;
			std::string label;
			std::string stage;
			bool hasStage = false;
		};

		struct FaultAnalysis
		{
			std::string likelyCause;
			std::string confidence;
			std::string confidenceReason;
			std::string resource;
			std::string location;
			std::vector<std::string> possibleCauses;
			std::vector<std::string> nextSteps;
		};

		struct PassWorkload
		{
			std::string name;
			std::uint64_t directDraws = 0;
			std::uint64_t indexedDraws = 0;
			std::uint64_t indirectDraws = 0;
			std::uint64_t indirectIndexedDraws = 0;
			std::uint64_t indirectCountDraws = 0;
			std::uint64_t dispatches = 0;
			std::uint64_t fillBuffers = 0;
			bool completed = false;
		};

		struct ActivitySummary
		{
			std::vector<PassWorkload> passes;
			std::vector<PassWorkload> previousPasses;
			std::optional<std::uint64_t> currentFrame;
			std::optional<std::uint64_t> previousFrame;
		};

		void TryInitAmdBufferMarker();
		void TryInitNvCheckpoints();
		std::vector<ResolvedBreadcrumb> CollectBreadcrumbs();
		std::vector<VkDeviceFaultInfoKHR> QueryFaultReports();
		std::string GetLabel(std::uint32_t markerValue) const;
		FaultAnalysis AnalyzeFaults(const std::vector<VkDeviceFaultInfoKHR>& faults, const std::vector<ResolvedBreadcrumb>& breadcrumbs) const;
		ActivitySummary BuildActivitySummary() const;
		std::string WorkloadText(const PassWorkload& pass) const;
		static PassWorkload* FindOrAddPass(std::vector<PassWorkload>& passes, std::string_view name);

		VkDevice m_device = VK_NULL_HANDLE;
		VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
		VkQueue m_graphicsQueue = VK_NULL_HANDLE;
		std::uint32_t m_vendorId = 0;
		std::string m_vendorName;
		std::string m_queueLabel = "Graphics";
		bool m_initialized = false;

		GpuMemoryTracker m_memoryTracker;

		// -- VK_KHR_device_fault function pointer
		PFN_vkGetDeviceFaultReportsKHR m_vkGetDeviceFaultReportsKHR = nullptr;

		// -- Flight recorder (VK_AMD_buffer_marker / VK_NV_device_diagnostic_checkpoints)
		bool m_hasBufferMarker = false;
		PFN_vkCmdWriteBufferMarker2AMD m_vkCmdWriteBufferMarker2AMD = nullptr;
		VkBuffer m_breadcrumbBuffer = VK_NULL_HANDLE;
		VkDeviceMemory m_breadcrumbMemory = VK_NULL_HANDLE;
		void* m_breadcrumbMapped = nullptr;

		bool m_hasCheckpointNV = false;
		PFN_vkCmdSetCheckpointNV m_vkCmdSetCheckpointNV = nullptr;
		PFN_vkGetQueueCheckpointDataNV m_vkGetQueueCheckpointDataNV = nullptr;

		static constexpr std::uint32_t kBreadcrumbSlotCount = 4096;
		std::atomic<std::uint64_t> m_nextBreadcrumbSlot{0};
		std::uint64_t m_currentFrameIndex = 0;
		std::atomic<bool> m_captureInProgress{false};

		// CPU-side label registry for marker values.
		mutable std::mutex m_labelMutex;
		std::unordered_map<std::uint32_t, std::string> m_breadcrumbLabels;

		static constexpr std::uint32_t kEventRingCount = 128;
		static constexpr std::size_t kEventMessageSize = 160;

		struct EventSlot
		{
			std::atomic<std::uint64_t> sequence{std::uint64_t(-1)};
			std::array<char, kEventMessageSize> message{};
		};

		std::array<EventSlot, kEventRingCount> m_eventSlots{};
		std::atomic<std::uint64_t> m_nextEventSlot{0};
	};
} // namespace aether
