#pragma once

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH

#	include <string_view>

#	include "utils/Assert.hpp"
#	include "vulkan/volk.hpp"

// volk.h includes vulkan_core.h but not vulkan.h. The Aftermath SDK guards
// SpirvCode / GetShaderHashSpirv behind VULKAN_H_ (from vulkan.h umbrella).
#	include <vulkan/vulkan.h>

#	include <GFSDK_Aftermath.h>
#	include <GFSDK_Aftermath_GpuCrashDump.h>
#	include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

namespace aether
{
	// Lightweight wrapper around NVIDIA Aftermath SDK for GPU crash diagnostics.
	//
	// Vulkan path:
	//   - Feature flags are configured via VK_NV_device_diagnostics_config
	//     (VkDeviceDiagnosticsConfigCreateInfoNV in the device pNext chain).
	//   - Event markers use VK_NV_device_diagnostic_checkpoints (vkCmdSetCheckpointNV).
	//   - There is no separate GFSDK_Aftermath_VK_InitializeDevice in this SDK version.
	//
	// Usage:
	//   1. Call EnableGpuCrashDumps(crashDumpDir) once, before VkDevice creation.
	//   2. Add VK_NV_device_diagnostics_config to device extension list
	//      with VkDeviceDiagnosticsConfigCreateInfoNV in pNext.
	//   3. Add VK_NV_device_diagnostic_checkpoints to device extension list
	//      for per-command-buffer event markers.
	//   4. After creating VkDevice, call Initialize(VkDevice, VkPhysicalDevice).
	//   5. Call SetEventMarker() on command buffers to add checkpoint breadcrumbs.
	//   6. Before shutdown call Shutdown() / DisableGpuCrashDumps().
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

		// Insert an event marker via vkCmdSetCheckpointNV.
		// Requires VK_NV_device_diagnostic_checkpoints to be enabled at device creation.
		void SetEventMarker(VkCommandBuffer cmd, std::string_view markerName) const;

		void Shutdown();
		void DisableGpuCrashDumps();

		[[nodiscard]] bool IsInitialized() const
		{
			return m_initialized;
		}

		[[nodiscard]] const std::string& GetCrashDumpDir() const
		{
			return m_crashDumpDir;
		}

		// Register SPIR-V binary for shader lookup during GPU crash dump decoding.
		// Must be called with the exact bytes passed to vkCreateShaderModule.
		static void RegisterShaderBinary(const void* pSpirv, uint32_t spirvSize);

	private:
		static void GFSDK_AFTERMATH_CALL OnCrashDump(const void* pGpuCrashDump, std::uint32_t gpuCrashDumpSize, void* pUserData);
		static void GFSDK_AFTERMATH_CALL OnShaderDebugInfo(const void* pShaderDebugInfo, std::uint32_t shaderDebugInfoSize, void* pUserData);
		static void GFSDK_AFTERMATH_CALL OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription, void* pUserData);
		static void GFSDK_AFTERMATH_CALL OnResolveMarker(const void* pMarkerData, std::uint32_t markerDataSize, void* pUserData, PFN_GFSDK_Aftermath_ResolveMarker resolveMarker);

		bool m_crashDumpsEnabled = false;
		bool m_initialized = false;
		VkDevice m_device = VK_NULL_HANDLE;
		std::string m_crashDumpDir;
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

		[[nodiscard]] const std::string& GetCrashDumpDir() const
		{
			return m_empty;
		}

		std::string m_empty;
	};
} // namespace aether

#endif // AETHER_ENABLE_NVIDIA_AFTERMATH
