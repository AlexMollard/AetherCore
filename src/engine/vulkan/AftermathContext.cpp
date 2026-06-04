#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH

#include "vulkan/AftermathContext.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		void WriteCrashDumpToDisk(const void* data, std::uint32_t size)
		{
			const auto now = std::chrono::system_clock::now();
			const auto timeT = std::chrono::system_clock::to_time_t(now);

			tm timeInfo;
			localtime_s(&timeInfo, &timeT);

			char timeBuf[64];
			std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &timeInfo);

			try
			{
				std::filesystem::path dir = std::filesystem::current_path() / "gpu_crash_dumps";
				std::filesystem::create_directories(dir);
				auto path = dir / (std::string("crash_") + timeBuf + ".nv-gpudmp");

				std::ofstream out(path, std::ios::binary);
				out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
				out.close();

				AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: GPU crash dump written to {}", path.string());
			}
			catch (...)
			{
				AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to write GPU crash dump to disk");
			}
		}
	} // namespace

	void GFSDK_AFTERMATH_CALL AftermathContext::OnCrashDump(const void* pGpuCrashDump, std::uint32_t gpuCrashDumpSize, void* /*pUserData*/)
	{
		AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: GPU crash dump received ({} bytes)", gpuCrashDumpSize);
		WriteCrashDumpToDisk(pGpuCrashDump, gpuCrashDumpSize);
	}

	void GFSDK_AFTERMATH_CALL AftermathContext::OnShaderDebugInfo(const void* pShaderDebugInfo, std::uint32_t shaderDebugInfoSize, void* /*pUserData*/)
	{
		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: shader debug info received ({} bytes)", shaderDebugInfoSize);
		if (pShaderDebugInfo && shaderDebugInfoSize > 0)
		{
			WriteCrashDumpToDisk(pShaderDebugInfo, shaderDebugInfoSize);
		}
	}

	void GFSDK_AFTERMATH_CALL AftermathContext::OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription, void* /*pUserData*/)
	{
		if (addDescription)
		{
			addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "AetherCore");
			addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "0.1.0");
		}
	}

	void GFSDK_AFTERMATH_CALL AftermathContext::OnResolveMarker(const void* pMarkerData, std::uint32_t markerDataSize, void* /*pUserData*/, PFN_GFSDK_Aftermath_ResolveMarker resolveMarker)
	{
		if (resolveMarker && pMarkerData)
		{
			resolveMarker(pMarkerData, markerDataSize);
		}
	}

	bool AftermathContext::EnableGpuCrashDumps(const char* /*crashDumpDir*/)
	{
		if (m_crashDumpsEnabled)
		{
			return true;
		}

		GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
		        GFSDK_Aftermath_Version_API,
		        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
		        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
		        OnCrashDump,
		        OnShaderDebugInfo,
		        OnDescription,
		        OnResolveMarker,
		        this);

		if (result != GFSDK_Aftermath_Result_Success)
		{
			AE_WARN(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to enable GPU crash dumps (result={})", static_cast<std::uint32_t>(result));
			return false;
		}

		m_crashDumpsEnabled = true;
		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: GPU crash dumps enabled");
		return true;
	}

	bool AftermathContext::Initialize(VkDevice device, VkPhysicalDevice /*physicalDevice*/)
	{
		if (m_initialized)
		{
			return true;
		}

		if (!m_crashDumpsEnabled)
		{
			return false;
		}

		m_device = device;
		m_initialized = true;
		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: initialized (VkDevice={})", static_cast<void*>(device));
		return true;
	}

	void AftermathContext::SetEventMarker(VkCommandBuffer /*cmd*/, std::string_view /*markerName*/) const
	{
		// The current Aftermath SDK uses typed GFSDK_Aftermath_ContextHandle,
		// which requires DX11/DX12-specific handle creation. For Vulkan, the
		// device diagnostics config extension provides automatic checkpoints.
	}

	void AftermathContext::Shutdown()
	{
		DisableGpuCrashDumps();
		m_device = VK_NULL_HANDLE;
		m_initialized = false;
	}

	void AftermathContext::DisableGpuCrashDumps()
	{
		if (!m_crashDumpsEnabled)
		{
			return;
		}

		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: Disabling GPU crash dumps");
		GFSDK_Aftermath_DisableGpuCrashDumps();
		m_crashDumpsEnabled = false;
	}
} // namespace aether

#endif // AETHER_ENABLE_NVIDIA_AFTERMATH
