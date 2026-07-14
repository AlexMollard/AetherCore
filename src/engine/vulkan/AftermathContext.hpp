#pragma once

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH

#	include <string_view>

#	include "utils/Assert.hpp"
#	include "vulkan/volk.hpp"

#	include <vulkan/vulkan.h>

#	include <GFSDK_Aftermath.h>
#	include <GFSDK_Aftermath_GpuCrashDump.h>
#	include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

namespace aether
{
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

		[[nodiscard]] const std::string& GetCrashDumpDir() const
		{
			return m_crashDumpDir;
		}

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

#endif
