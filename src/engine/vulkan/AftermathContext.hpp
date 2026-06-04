#pragma once

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH

#include <string_view>

#include "utils/Assert.hpp"
#include "vulkan/volk.hpp"

#include <GFSDK_Aftermath.h>
#include <GFSDK_Aftermath_GpuCrashDump.h>

namespace aether
{
	// Lightweight wrapper around NVIDIA Aftermath SDK for GPU crash diagnostics.
	//
	// Usage:
	//   1. Call EnableGpuCrashDumps() once, early (before VkDevice creation).
	//   2. After creating VkDevice, call Initialize(VkDevice, VkPhysicalDevice).
	//   3. Before shutdown call Shutdown() / DisableGpuCrashDumps().
	//
	// Resource tracking and shader debug info are enabled via the
	// VK_NV_device_diagnostics_config extension during device creation.
	class AftermathContext
	{
	public:
		AftermathContext() = default;

		~AftermathContext()
		{
			Shutdown();
		}

		AftermathContext(const AftermathContext&) = delete;
		AftermathContext& operator=(const AftermathContext&) = delete;

		[[nodiscard]] bool EnableGpuCrashDumps(const char* crashDumpDir);
		[[nodiscard]] bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice);

		void SetEventMarker(VkCommandBuffer cmd, std::string_view markerName) const;

		void Shutdown();
		void DisableGpuCrashDumps();

		[[nodiscard]] bool IsInitialized() const
		{
			return m_initialized;
		}

	private:
		static void GFSDK_AFTERMATH_CALL OnCrashDump(const void* pGpuCrashDump, std::uint32_t gpuCrashDumpSize, void* pUserData);
		static void GFSDK_AFTERMATH_CALL OnShaderDebugInfo(const void* pShaderDebugInfo, std::uint32_t shaderDebugInfoSize, void* pUserData);
		static void GFSDK_AFTERMATH_CALL OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription, void* pUserData);
		static void GFSDK_AFTERMATH_CALL OnResolveMarker(const void* pMarkerData, std::uint32_t markerDataSize, void* pUserData, PFN_GFSDK_Aftermath_ResolveMarker resolveMarker);

		bool m_crashDumpsEnabled = false;
		bool m_initialized = false;
		VkDevice m_device = VK_NULL_HANDLE;
	};
} // namespace aether

#else

// Stub for builds without Aftermath.
namespace aether
{
	class AftermathContext
	{
	public:
		AftermathContext() = default;
		~AftermathContext() = default;

		[[nodiscard]] bool EnableGpuCrashDumps(const char*)
		{
			return false;
		}
		[[nodiscard]] bool Initialize(VkDevice, VkPhysicalDevice)
		{
			return false;
		}
		void SetEventMarker(VkCommandBuffer, std::string_view) const
		{
		}
		void Shutdown()
		{
		}
		void DisableGpuCrashDumps()
		{
		}
		[[nodiscard]] bool IsInitialized() const
		{
			return false;
		}
	};
} // namespace aether

#endif // AETHER_ENABLE_NVIDIA_AFTERMATH
