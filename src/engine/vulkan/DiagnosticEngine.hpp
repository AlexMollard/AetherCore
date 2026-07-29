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
#include "vulkan/DeviceFaultQuery.hpp"
#include "vulkan/GpuMemoryTracker.hpp"

namespace aether
{
	class DiagnosticEngine
	{
	public:
		DiagnosticEngine() = default;
		~DiagnosticEngine() = default;
		DiagnosticEngine(const DiagnosticEngine&) = delete;
		DiagnosticEngine& operator=(const DiagnosticEngine&) = delete;

		void Init(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue);

		void Shutdown();

		void CaptureFaults();

		[[nodiscard]] GpuMemoryTracker& GetMemoryTracker()
		{
			return m_memoryTracker;
		}

		void RegisterResource(VkDeviceAddress addr, VkDeviceSize size, std::string_view name, GpuMemoryTracker::ResourceType type)
		{
			m_memoryTracker.Register(addr, size, std::string(name), type);
		}

		void UnregisterResource(VkDeviceAddress addr)
		{
			m_memoryTracker.Unregister(addr);
		}

		void WriteBreadcrumb(VkCommandBuffer cmd, std::uint32_t markerValue);

		void RegisterBreadcrumbLabel(std::uint32_t markerValue, std::string_view label);

		void BeginFrame(std::uint64_t frameIndex);
		void EndFrame(std::uint64_t frameIndex);

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
		std::string GetLabel(std::uint32_t markerValue) const;
		FaultAnalysis AnalyzeFault(const std::optional<vulkan::DeviceFaultReport>& fault, const std::vector<ResolvedBreadcrumb>& breadcrumbs) const;
		ActivitySummary BuildActivitySummary() const;
		static std::string WorkloadText(const PassWorkload& pass);
		static PassWorkload* FindOrAddPass(std::vector<PassWorkload>& passes, std::string_view name);

		VkDevice m_device = VK_NULL_HANDLE;
		VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
		VkQueue m_graphicsQueue = VK_NULL_HANDLE;
		std::uint32_t m_vendorId = 0;
		std::string m_vendorName;
		std::string m_queueLabel = "Graphics";
		bool m_initialized = false;

		GpuMemoryTracker m_memoryTracker;

		PFN_vkGetDeviceFaultInfoEXT m_vkGetDeviceFaultInfoEXT = nullptr;

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
